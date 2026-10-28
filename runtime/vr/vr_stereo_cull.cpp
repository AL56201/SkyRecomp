#include "vr_stereo_cull.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
enum { PLANE_LEFT, PLANE_RIGHT, PLANE_BOTTOM, PLANE_TOP, PLANE_NEAR, PLANE_FAR };

bool finite_matrix(const float matrix[16]) {
    if (!matrix) return false;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(matrix[i])) return false;
    return true;
}

void normalize_plane(float plane[4]) {
    const float length = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
    if (!std::isfinite(length) || length <= 1.0e-7f) {
        std::memset(plane, 0, sizeof(float) * 4);
        return;
    }
    for (int i = 0; i < 4; ++i) plane[i] /= length;
}

void combine_rows(float out[4], const float matrix[16], int first, int second, float second_scale) {
    for (int column = 0; column < 4; ++column)
        out[column] = matrix[first * 4 + column] + second_scale * matrix[second * 4 + column];
    normalize_plane(out);
}
} // namespace

extern "C" void ComputeStereoProjectionMatrix(float out[16], const VRStereoEyeParams *eye, float near_z,
                                                float far_z) {
    if (!out) return;
    std::memset(out, 0, sizeof(float) * 16);
    if (!eye) return;
    const float near_plane = near_z > 0.0f ? near_z : eye->near_z;
    const float far_plane = far_z > 0.0f ? far_z : eye->far_z;
    const float left = std::tan(eye->angle_left);
    const float right = std::tan(eye->angle_right);
    const float top = std::tan(eye->angle_up);
    const float bottom = std::tan(eye->angle_down);
    const float width = right - left, height = top - bottom;
    if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(top) || !std::isfinite(bottom) ||
        !std::isfinite(near_plane) || !std::isfinite(far_plane) || near_plane <= 0.0f ||
        far_plane <= near_plane || width <= 1.0e-7f || height <= 1.0e-7f)
        return;

    out[0] = 2.0f / width;
    out[2] = (right + left) / width;
    out[5] = 2.0f / height;
    out[6] = (top + bottom) / height;
    out[10] = far_plane / (near_plane - far_plane);
    out[11] = (far_plane * near_plane) / (near_plane - far_plane);
    out[14] = -1.0f;
}

extern "C" void ExpandViewFrustumForWideFOV(float out[6][4], const float matrix[16], float expansion_factor) {
    if (!out) return;
    std::memset(out, 0, sizeof(float) * 6 * 4);
    if (!finite_matrix(matrix)) return;
    const float expansion = std::isfinite(expansion_factor) ? std::max(expansion_factor, 1.0f) : 1.0f;
    float widened[16];
    std::memcpy(widened, matrix, sizeof widened);
    for (int column = 0; column < 4; ++column) {
        widened[column] /= expansion;       // clip X row
        widened[4 + column] /= expansion;   // clip Y row
    }

    // -w <= x <= w, -w <= y <= w, and Vulkan's 0 <= z <= w.
    combine_rows(out[PLANE_LEFT], widened, 3, 0, 1.0f);
    combine_rows(out[PLANE_RIGHT], widened, 3, 0, -1.0f);
    combine_rows(out[PLANE_BOTTOM], widened, 3, 1, 1.0f);
    combine_rows(out[PLANE_TOP], widened, 3, 1, -1.0f);
    for (int column = 0; column < 4; ++column) out[PLANE_NEAR][column] = widened[8 + column];
    normalize_plane(out[PLANE_NEAR]);
    combine_rows(out[PLANE_FAR], widened, 3, 2, -1.0f);
}

extern "C" bool TestSphereAgainstStereoFrustum(const float planes[6][4], const float center[3], float radius) {
    if (!planes || !center || !std::isfinite(radius)) return false;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(center[i])) return false;
    radius = std::max(radius, 0.0f);
    for (int plane = 0; plane < 6; ++plane) {
        const float normal_length_squared = planes[plane][0] * planes[plane][0] +
                                           planes[plane][1] * planes[plane][1] +
                                           planes[plane][2] * planes[plane][2];
        if (normal_length_squared <= 1.0e-12f) return false;
        const float distance = planes[plane][0] * center[0] + planes[plane][1] * center[1] +
                               planes[plane][2] * center[2] + planes[plane][3];
        if (distance < -radius) return false;
    }
    return true;
}

extern "C" bool ValidateVulkanToOpenXRFormat(uint32_t source, uint32_t destination, bool *needs_swizzle,
                                               bool *is_srgb) {
    const auto format_info = [](uint32_t format, bool &bgra, bool &srgb) {
        switch (format) {
        case VR_VK_FORMAT_R8G8B8A8_UNORM: bgra = false; srgb = false; return true;
        case VR_VK_FORMAT_R8G8B8A8_SRGB: bgra = false; srgb = true; return true;
        case VR_VK_FORMAT_B8G8R8A8_UNORM: bgra = true; srgb = false; return true;
        case VR_VK_FORMAT_B8G8R8A8_SRGB: bgra = true; srgb = true; return true;
        default: bgra = false; srgb = false; return false;
        }
    };
    bool source_bgra = false, source_srgb = false, destination_bgra = false, destination_srgb = false;
    const bool source_known = format_info(source, source_bgra, source_srgb);
    const bool destination_known = format_info(destination, destination_bgra, destination_srgb);
    if (needs_swizzle) *needs_swizzle = source_known && destination_known && source_bgra != destination_bgra;
    if (is_srgb) *is_srgb = destination_known && destination_srgb;
    return source_known && destination_known && source_srgb == destination_srgb;
}

extern "C" void ConvertRGBAToBGRASwizzle(uint8_t *pixels, size_t pixel_count) {
    if (!pixels) return;
    for (size_t pixel = 0; pixel < pixel_count; ++pixel)
        std::swap(pixels[pixel * 4], pixels[pixel * 4 + 2]);
}

extern "C" float LinearToSRGB(float linear) {
    linear = std::clamp(std::isfinite(linear) ? linear : 0.0f, 0.0f, 1.0f);
    return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

extern "C" float SRGBToLinear(float srgb) {
    srgb = std::clamp(std::isfinite(srgb) ? srgb : 0.0f, 0.0f, 1.0f);
    return srgb <= 0.04045f ? srgb / 12.92f : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
}
