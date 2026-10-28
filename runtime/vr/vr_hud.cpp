#include "vr_hud.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "imgui.h"

namespace {
constexpr VRHUDConfig kDefaults = {2.0f, 1.0f, 0.0f, false, false, true, false, false,
                                   0.20f, 0.12f, 0.02f, 0.10f, 0.0f, 0.46f};
constexpr float kFollowTime = 0.25f;
constexpr float kMaxDelta = 0.1f;
constexpr float kCurveAngle = 0.35f; // radians from the centre to either edge

struct FollowState {
    bool valid = false;
    float position[3] = {};
    float forward[3] = {0.0f, 0.0f, -1.0f};
};
FollowState follow;

void identity(float out[16]) {
    std::memset(out, 0, sizeof(float) * 16);
    out[0] = out[5] = out[10] = out[15] = 1.0f;
}

void multiply(float out[16], const float a[16], const float b[16]) {
    float result[16];
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) {
            result[row * 4 + column] = 0.0f;
            for (int k = 0; k < 4; ++k) result[row * 4 + column] += a[row * 4 + k] * b[k * 4 + column];
        }
    std::memcpy(out, result, sizeof result);
}

float length3(const float value[3]) {
    return std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
}

bool normalize(float value[3]) {
    const float length = length3(value);
    if (!std::isfinite(length) || length < 1.0e-5f) return false;
    for (int i = 0; i < 3; ++i) value[i] /= length;
    return true;
}

void cross(float out[3], const float a[3], const float b[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

void sanitize_config(void) {
    if (!std::isfinite(g_vr_hud_config.hud_distance)) g_vr_hud_config.hud_distance = kDefaults.hud_distance;
    if (!std::isfinite(g_vr_hud_config.hud_scale)) g_vr_hud_config.hud_scale = kDefaults.hud_scale;
    if (!std::isfinite(g_vr_hud_config.hud_vertical_offset))
        g_vr_hud_config.hud_vertical_offset = kDefaults.hud_vertical_offset;
    g_vr_hud_config.hud_distance = std::clamp(g_vr_hud_config.hud_distance, 0.5f, 10.0f);
    g_vr_hud_config.hud_scale = std::clamp(g_vr_hud_config.hud_scale, 0.25f, 3.0f);
    g_vr_hud_config.hud_vertical_offset = std::clamp(g_vr_hud_config.hud_vertical_offset, -2.0f, 2.0f);
    if (!std::isfinite(g_vr_hud_config.wrist_hud_size)) g_vr_hud_config.wrist_hud_size = kDefaults.wrist_hud_size;
    if (!std::isfinite(g_vr_hud_config.wrist_hud_side)) g_vr_hud_config.wrist_hud_side = kDefaults.wrist_hud_side;
    if (!std::isfinite(g_vr_hud_config.wrist_hud_up)) g_vr_hud_config.wrist_hud_up = kDefaults.wrist_hud_up;
    if (!std::isfinite(g_vr_hud_config.wrist_hud_forward)) g_vr_hud_config.wrist_hud_forward = kDefaults.wrist_hud_forward;
    if (!std::isfinite(g_vr_hud_config.wrist_hud_tilt)) g_vr_hud_config.wrist_hud_tilt = kDefaults.wrist_hud_tilt;
    if (!std::isfinite(g_vr_hud_config.wrist_hud_background)) g_vr_hud_config.wrist_hud_background = kDefaults.wrist_hud_background;
    g_vr_hud_config.wrist_hud_size = std::clamp(g_vr_hud_config.wrist_hud_size, 0.08f, 0.50f);
    g_vr_hud_config.wrist_hud_side = std::clamp(g_vr_hud_config.wrist_hud_side, -0.20f, 0.20f);
    g_vr_hud_config.wrist_hud_up = std::clamp(g_vr_hud_config.wrist_hud_up, -0.20f, 0.20f);
    g_vr_hud_config.wrist_hud_forward = std::clamp(g_vr_hud_config.wrist_hud_forward, -0.20f, 0.25f);
    g_vr_hud_config.wrist_hud_tilt = std::clamp(g_vr_hud_config.wrist_hud_tilt, -90.0f, 90.0f);
    g_vr_hud_config.wrist_hud_background = std::clamp(g_vr_hud_config.wrist_hud_background, 0.0f, 0.85f);
}

void composed_head(float out[16], const float head_pose[16], const float camera_pose[16]) {
    float head_identity[16], camera_identity[16];
    if (!head_pose) { identity(head_identity); head_pose = head_identity; }
    if (!camera_pose) { identity(camera_identity); camera_pose = camera_identity; }
    multiply(out, camera_pose, head_pose);
}

void build_matrix(float out[16], const float position[3], const float right[3], const float up[3],
                  const float back[3]) {
    identity(out);
    const float scale = g_vr_hud_config.hud_scale;
    for (int row = 0; row < 3; ++row) {
        out[row * 4 + 0] = right[row] * scale;
        out[row * 4 + 1] = up[row] * scale;
        out[row * 4 + 2] = back[row] * scale;
        out[row * 4 + 3] = position[row];
    }
}
} // namespace

VRHUDConfig g_vr_hud_config = kDefaults;

extern "C" void ResetVRHUDConfig(void) {
    g_vr_hud_config = kDefaults;
    ResetHUDLazyFollow();
}

extern "C" void ValidateVRHUDConfig(void) { sanitize_config(); }

extern "C" void ResetHUDLazyFollow(void) { follow = FollowState{}; }

// While the menu pointer is in use (vr_controls.c: title, file select, paused menus) the panel holds still where it
// was placed (in front of the head when the hold began): with the panel following the head, the same controller
// direction pointed at different parts of the menu as the head turned, and the cursor jumped across it.
bool g_vr_hud_hold;
static bool held;

extern "C" void ComputeHUDQuadMatrixDelta(float out[16], const float head_pose[16], const float camera_pose[16],
                                            float delta_seconds) {
    if (!out) return;
    sanitize_config();

    float head[16];
    composed_head(head, head_pose, camera_pose);
    const float head_position[3] = {head[3], head[7], head[11]};
    float head_right[3] = {head[0], head[4], head[8]};
    float head_up[3] = {head[1], head[5], head[9]};
    float head_forward[3] = {-head[2], -head[6], -head[10]};
    if (!normalize(head_right)) { head_right[0] = 1.0f; head_right[1] = head_right[2] = 0.0f; }
    if (!normalize(head_up)) { head_up[0] = head_up[2] = 0.0f; head_up[1] = 1.0f; }
    if (!normalize(head_forward)) { head_forward[0] = head_forward[1] = 0.0f; head_forward[2] = -1.0f; }

    if (g_vr_hud_hold != held) {  // (the hold begins: placed in front of the head again; ends: follows again)
        held = g_vr_hud_hold;
        if (held) follow.valid = false;
    }
    if (g_vr_hud_config.lock_to_head && !held) {
        float position[3];
        for (int i = 0; i < 3; ++i)
            position[i] = head_position[i] + head_forward[i] * g_vr_hud_config.hud_distance +
                          head_up[i] * g_vr_hud_config.hud_vertical_offset;
        const float back[3] = {-head_forward[0], -head_forward[1], -head_forward[2]};
        build_matrix(out, position, head_right, head_up, back);
        follow.valid = false;
        return;
    }

    // A world-locked HUD remains level. Only the viewer's yaw affects its target orientation.
    float target_forward[3] = {head_forward[0], 0.0f, head_forward[2]};
    if (!normalize(target_forward)) {
        target_forward[0] = follow.valid ? follow.forward[0] : 0.0f;
        target_forward[1] = 0.0f;
        target_forward[2] = follow.valid ? follow.forward[2] : -1.0f;
    }
    float target_position[3];
    for (int i = 0; i < 3; ++i)
        target_position[i] = head_position[i] + target_forward[i] * g_vr_hud_config.hud_distance;
    target_position[1] += g_vr_hud_config.hud_vertical_offset;

    if (!follow.valid) {
        std::memcpy(follow.position, target_position, sizeof target_position);
        std::memcpy(follow.forward, target_forward, sizeof target_forward);
        follow.valid = true;
    } else if (!held) {
        const float dt = std::clamp(std::isfinite(delta_seconds) ? delta_seconds : 0.0f, 0.0f, kMaxDelta);
        const float blend = 1.0f - std::exp(-dt / kFollowTime);
        for (int i = 0; i < 3; ++i) {
            follow.position[i] += (target_position[i] - follow.position[i]) * blend;
            follow.forward[i] += (target_forward[i] - follow.forward[i]) * blend;
        }
        if (!normalize(follow.forward)) std::memcpy(follow.forward, target_forward, sizeof target_forward);
    }

    const float world_up[3] = {0.0f, 1.0f, 0.0f};
    float right[3];
    cross(right, follow.forward, world_up);
    if (!normalize(right)) { right[0] = 1.0f; right[1] = right[2] = 0.0f; }
    float up[3];
    cross(up, right, follow.forward);
    normalize(up);
    const float back[3] = {-follow.forward[0], -follow.forward[1], -follow.forward[2]};
    build_matrix(out, follow.position, right, up, back);
}

extern "C" void ComputeHUDQuadMatrix(float out[16], const float head_pose[16], const float camera_pose[16]) {
    ComputeHUDQuadMatrixDelta(out, head_pose, camera_pose, 1.0f / 90.0f);
}

extern "C" void ComputeHUDSurfacePoint(float out[3], float x, float y) {
    if (!out) return;
    sanitize_config();
    x = std::clamp(x, -1.0f, 1.0f);
    y = std::clamp(y, -1.0f, 1.0f);
    if (!g_vr_hud_config.curvature) { out[0] = x; out[1] = y; out[2] = 0.0f; return; }
    const float angle = x * kCurveAngle;
    const float radius = 1.0f / kCurveAngle;
    out[0] = std::sin(angle) * radius;
    out[1] = y;
    out[2] = radius * (1.0f - std::cos(angle));
}

extern "C" void RenderVRHUDControls(void) {
    ImGui::TextUnformatted("HUD Projection");
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("HUD distance", &g_vr_hud_config.hud_distance, 0.5f, 10.0f, "%.2f m");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("HUD scale", &g_vr_hud_config.hud_scale, 0.25f, 3.0f, "%.2fx");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("HUD vertical offset", &g_vr_hud_config.hud_vertical_offset, -2.0f, 2.0f, "%.2f m");
    ImGui::Checkbox("Curved HUD", &g_vr_hud_config.curvature);
    if (ImGui::Checkbox("Lock HUD to head", &g_vr_hud_config.lock_to_head)) ResetHUDLazyFollow();
    ImGui::Spacing();
    ImGui::TextUnformatted("Wrist HUD");
    ImGui::Separator();
    ImGui::Checkbox("Use wrist HUD during gameplay", &g_vr_hud_config.wrist_hud);
    ImGui::BeginDisabled(!g_vr_hud_config.wrist_hud);
    ImGui::Checkbox("Show status only when looking", &g_vr_hud_config.wrist_hud_glance);
    ImGui::Checkbox("Swap status and item hands", &g_vr_hud_config.wrist_hud_swap_hands);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("Wrist HUD size", &g_vr_hud_config.wrist_hud_size, 0.08f, 0.50f, "%.2f m");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("Wrist HUD sideways", &g_vr_hud_config.wrist_hud_side, -0.20f, 0.20f, "%.2f m");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("Wrist HUD up", &g_vr_hud_config.wrist_hud_up, -0.20f, 0.20f, "%.2f m");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("Wrist HUD along forearm", &g_vr_hud_config.wrist_hud_forward, -0.20f, 0.25f, "%.2f m");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("Wrist HUD tilt", &g_vr_hud_config.wrist_hud_tilt, -90.0f, 90.0f, "%.0f deg");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("Wrist background opacity", &g_vr_hud_config.wrist_hud_background, 0.0f, 0.85f, "%.2f");
    if (ImGui::Button("Reset wrist HUD placement")) {
        g_vr_hud_config.wrist_hud_size = kDefaults.wrist_hud_size;
        g_vr_hud_config.wrist_hud_side = kDefaults.wrist_hud_side;
        g_vr_hud_config.wrist_hud_up = kDefaults.wrist_hud_up;
        g_vr_hud_config.wrist_hud_forward = kDefaults.wrist_hud_forward;
        g_vr_hud_config.wrist_hud_tilt = kDefaults.wrist_hud_tilt;
        g_vr_hud_config.wrist_hud_background = kDefaults.wrist_hud_background;
    }
    ImGui::EndDisabled();
}
