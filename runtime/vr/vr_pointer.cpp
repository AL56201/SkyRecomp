#include "vr_pointer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr float kEpsilon = 1.0e-6f;

float dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void subtract(float out[3], const float a[3], const float b[3]) {
    for (int i = 0; i < 3; ++i) out[i] = a[i] - b[i];
}

void cross(float out[3], const float a[3], const float b[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

bool normalize(float value[3]) {
    const float length_squared = dot(value, value);
    if (!std::isfinite(length_squared) || length_squared <= kEpsilon * kEpsilon) return false;
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    for (int i = 0; i < 3; ++i) value[i] *= inverse_length;
    return true;
}

bool finite_matrix(const float matrix[16]) {
    if (!matrix) return false;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(matrix[i])) return false;
    return true;
}
} // namespace

extern "C" bool ComputeVRPointerIntersection(VRPointerState *out, const float controller[16], const float hud[16]) {
    if (!out) return false;
    std::memset(out, 0, sizeof *out);
    if (!finite_matrix(controller) || !finite_matrix(hud)) return false;

    out->active = true;
    out->ray_origin[0] = controller[3];
    out->ray_origin[1] = controller[7];
    out->ray_origin[2] = controller[11];
    out->ray_direction[0] = -controller[2];
    out->ray_direction[1] = -controller[6];
    out->ray_direction[2] = -controller[10];
    if (!normalize(out->ray_direction)) { out->active = false; return false; }

    const float centre[3] = {hud[3], hud[7], hud[11]};
    const float axis_x[3] = {hud[0], hud[4], hud[8]};
    const float axis_y[3] = {hud[1], hud[5], hud[9]};
    float normal[3];
    cross(normal, axis_x, axis_y);
    if (!normalize(normal)) return false;

    const float denominator = dot(out->ray_direction, normal);
    if (std::fabs(denominator) <= kEpsilon) return false;
    float centre_from_origin[3];
    subtract(centre_from_origin, centre, out->ray_origin);
    const float distance = dot(centre_from_origin, normal) / denominator;
    if (!std::isfinite(distance) || distance < 0.0f) return false;

    float relative_hit[3];
    for (int i = 0; i < 3; ++i)
        relative_hit[i] = out->ray_origin[i] + out->ray_direction[i] * distance - centre[i];

    // Solve the 2x2 Gram system. This remains correct if a caller supplies scaled or mildly skewed HUD axes.
    const float xx = dot(axis_x, axis_x), xy = dot(axis_x, axis_y), yy = dot(axis_y, axis_y);
    const float determinant = xx * yy - xy * xy;
    if (!std::isfinite(determinant) || std::fabs(determinant) <= kEpsilon) return false;
    const float rx = dot(relative_hit, axis_x), ry = dot(relative_hit, axis_y);
    const float local_x = (rx * yy - ry * xy) / determinant;
    const float local_y = (ry * xx - rx * xy) / determinant;
    if (local_x < -1.0f - kEpsilon || local_x > 1.0f + kEpsilon ||
        local_y < -1.0f - kEpsilon || local_y > 1.0f + kEpsilon)
        return false;

    out->ui_space_x = std::clamp(local_x * 0.5f + 0.5f, 0.0f, 1.0f);
    out->ui_space_y = std::clamp(0.5f - local_y * 0.5f, 0.0f, 1.0f);
    out->distance_to_quad = distance;
    out->intersects = true;
    return true;
}

extern "C" bool ComputeVRPointerBeamSegment(float out[6], const VRPointerState *pointer, float max_distance) {
    if (!out || !pointer || !pointer->active) return false;
    const float fallback = std::isfinite(max_distance) ? std::max(max_distance, 0.0f) : 0.0f;
    const float length = pointer->intersects ? pointer->distance_to_quad : fallback;
    for (int i = 0; i < 3; ++i) {
        out[i] = pointer->ray_origin[i];
        out[i + 3] = pointer->ray_origin[i] + pointer->ray_direction[i] * length;
    }
    return true;
}
