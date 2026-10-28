#include "StereoCameraHook.h"

#include <cmath>
#include <cstring>

namespace skyrecomp::vr {
namespace {

using Quat = std::array<float, 4>;  // x, y, z, w
using Vec3 = std::array<float, 3>;

Quat conjugate(const Quat &q) noexcept { return {-q[0], -q[1], -q[2], q[3]}; }

Quat multiply(const Quat &a, const Quat &b) noexcept {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

bool normalize(Quat &q) noexcept {
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 0.5f) || !std::isfinite(n)) return false;  // runtimes report unit quaternions; reject garbage
    for (float &c : q) c /= n;
    return true;
}

// Row-major 3x3 rotation of a unit quaternion.
std::array<float, 9> rotation(const Quat &q) noexcept {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
            2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
            2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
}

Vec3 rotate(const std::array<float, 9> &r, const Vec3 &v) noexcept {
    return {r[0] * v[0] + r[1] * v[1] + r[2] * v[2],
            r[3] * v[0] + r[4] * v[1] + r[5] * v[2],
            r[6] * v[0] + r[7] * v[1] + r[8] * v[2]};
}

bool near(float v, float want, float tolerance) noexcept { return std::fabs(v - want) < tolerance; }

}  // namespace

bool StereoCameraHook::isSceneProjection(const float *u, std::size_t bytes) noexcept {
    if (!u || bytes < sizeof(CameraUbo)) return false;
    for (int i = 0; i < SKY_VR_CAMERA_FLOATS; i++)
        if (!std::isfinite(u[i])) return false;
    // Perspective projection; orthographic UI / layout / shadow blocks fail on P[3][2] and P[3][3].
    return near(u[1], 0, 1e-5f) && near(u[3], 0, 1e-5f) && near(u[4], 0, 1e-5f) && near(u[7], 0, 1e-5f) &&
           near(u[8], 0, 1e-5f) && near(u[9], 0, 1e-5f) && near(u[11], 1, 1e-3f) && near(u[12], 0, 1e-5f) &&
           near(u[13], 0, 1e-5f) && near(u[14], -1, 1e-3f) && near(u[15], 0, 1e-5f) && u[0] > 0.1f && u[0] < 10.0f &&
           std::fabs(u[5]) > 0.1f && std::fabs(u[5]) < 10.0f;
}

bool StereoCameraHook::isSceneCamera(const float *u, std::size_t bytes) noexcept {
    const bool projection = isSceneProjection(u, bytes);
    if (!projection) return false;
    // The view's rotation part is orthonormal (rows 16..18, 20..22, 24..26).
    const float *a = u + 16, *b = u + 20, *c = u + 24;
    const auto dot = [](const float *p, const float *q) { return p[0] * q[0] + p[1] * q[1] + p[2] * q[2]; };
    return near(dot(a, a), 1, 0.03f) && near(dot(b, b), 1, 0.03f) && near(dot(c, c), 1, 0.03f) &&
           near(dot(a, b), 0, 0.03f) && near(dot(a, c), 0, 0.03f) && near(dot(b, c), 0, 0.03f);
}

bool StereoCameraHook::eyeCamera(const CameraUbo &mono, const Pose &reference, const Pose &eye, const Fov &fov,
                                 float unitsPerMetre, CameraUbo &out) noexcept {
    if (!isSceneCamera(reinterpret_cast<const float *>(&mono), sizeof mono) ||!(unitsPerMetre > 0) || !std::isfinite(unitsPerMetre))
        return false;
    const float l = std::tan(fov.left), r = std::tan(fov.right), d = std::tan(fov.down), u = std::tan(fov.up);
    if (!(r - l > 0.05f) || !(u - d > 0.05f) || !std::isfinite(r - l) || !std::isfinite(u - d)) return false;

    // The eye's pose relative to the reference head pose, in the reference's own axes. Those axes are the game
    // camera's eye space, so the eye transform composes directly after the game's view.
    Quat ref_q = reference.orientation, eye_q = eye.orientation;
    if (!normalize(ref_q) || !normalize(eye_q)) return false;
    const Quat ref_inv = conjugate(ref_q);
    const auto rel = rotation(multiply(ref_inv, eye_q));
    const Vec3 world_delta = {eye.position[0] - reference.position[0], eye.position[1] - reference.position[1],
                              eye.position[2] - reference.position[2]};
    Vec3 offset = rotate(rotation(ref_inv), world_delta);
    for (float &v : offset) v *= unitsPerMetre;

    // E = inverse([rel | offset]) = [rel^T | -rel^T offset]; V' = E * [V; 0 0 0 1].
    const float e[12] = {rel[0], rel[3], rel[6], -(rel[0] * offset[0] + rel[3] * offset[1] + rel[6] * offset[2]),
                         rel[1], rel[4], rel[7], -(rel[1] * offset[0] + rel[4] * offset[1] + rel[7] * offset[2]),
                         rel[2], rel[5], rel[8], -(rel[2] * offset[0] + rel[5] * offset[1] + rel[8] * offset[2])};
    CameraUbo result = mono;
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 4; col++) {
            float v = e[row * 4 + 3] * (col == 3 ? 1.0f : 0.0f);
            for (int k = 0; k < 3; k++) v += e[row * 4 + k] * mono.view[k * 4 + col];
            result.view[row * 4 + col] = v;
        }

    return withView(mono, mono.view, result.view, fov, 0.0f, out);
}

bool StereoCameraHook::withView(const CameraUbo &mono, const float gameView[12], const float view[12], const Fov &fov,
                                float nearUnits, CameraUbo &out) noexcept {
    if (!isSceneProjection(reinterpret_cast<const float *>(&mono), sizeof mono) || !view || !gameView) return false;
    for (int i = 0; i < 12; i++)
        if (!std::isfinite(view[i]) || !std::isfinite(gameView[i])) return false;
    const float l = std::tan(fov.left), r = std::tan(fov.right), d = std::tan(fov.down), u = std::tan(fov.up);
    if (!(r - l > 0.05f) || !(u - d > 0.05f) || !std::isfinite(r - l) || !std::isfinite(u - d)) return false;
    // Eye projection: the asymmetric headset frustum. The depth rows stay the game's (its depth range and reversed-Z
    // convention), and the sign of P[1][1] is kept so the eye image has the same Y orientation and triangle winding
    // as the mono one.
    float pe[16];
    std::memcpy(pe, mono.projection, sizeof pe);
    const float y_sign = mono.projection[5] < 0 ? -1.0f : 1.0f;
    pe[0] = 2.0f / (r - l);
    pe[2] = (r + l) / (r - l);
    pe[5] = y_sign * 2.0f / (u - d);
    pe[6] = y_sign * (u + d) / (u - d);
    // Near plane: depth = (P[2][2] z + P[2][3]) / -z reaches the far side of the depth range at -z = P[2][3]
    // (the game's near distance); scaling the row moves that to nearUnits, so nearer geometry is clipped.
    const float gameNear = mono.projection[11];
    if (nearUnits > gameNear && gameNear > 0 && std::isfinite(nearUnits))
        for (int i = 8; i < 12; i++) pe[i] *= nearUnits / gameNear;

    // The view stays the block's; the eye goes into the projection instead: P' = Pe * C, C = Ve * Vg^-1 (game camera
    // space -> eye space), Vg the game's camera. A block's own view is Vg times a model matrix for many draws (terrain
    // chunks with a local origin, the scaled sky dome): C must come from the camera, not from that block. Animated characters are skinned with bone matrices the game already multiplied by its
    // own view (vertex slots 3..5) and only take the projection from this block: replacing the view would leave
    // them where they stand relative to the game camera (floating, in 1st person). This way static geometry gets
    // Pe * Ve and characters get Pe * C * (their game-camera-space positions): both in the eye's view, and lighting
    // or fog the shaders compute in game-camera space stays consistent with them.
    const float *g = gameView;
    const float ginv[12] = {g[0], g[4], g[8], -(g[0] * g[3] + g[4] * g[7] + g[8] * g[11]),
                            g[1], g[5], g[9], -(g[1] * g[3] + g[5] * g[7] + g[9] * g[11]),
                            g[2], g[6], g[10], -(g[2] * g[3] + g[6] * g[7] + g[10] * g[11])};
    float c[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 4; col++) {
            float v = col == 3 ? view[row * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; k++) v += view[row * 4 + k] * ginv[k * 4 + col];
            c[row * 4 + col] = v;
        }
    CameraUbo result = mono;
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += pe[row * 4 + k] * c[k * 4 + col];
            result.projection[row * 4 + col] = v;
        }
    for (float v : result.projection)
        if (!std::isfinite(v)) return false;
    out = result;
    return true;
}

bool StereoCameraHook::direct(const CameraUbo &mono, const float view[12], const Fov &fov, float nearUnits,
                              CameraUbo &out) noexcept {
    // Fragment-stage camera blocks are scanned across every uniform slot. A
    // projected shadow/decal can contain a perspective-shaped matrix followed
    // by a light/projector transform, so checking the projection alone would
    // mistake that block for the scene camera and fold the headset eye into the
    // projector. Its finite quad bounds then become visible on the ground.
    // Real screen-space reflection/refraction cameras have an orthonormal view.
    if (!isSceneCamera(reinterpret_cast<const float *>(&mono), sizeof mono)) return false;
    // With identical game and eye views, withView produces the headset projection Pe (C is identity). Replace the
    // preserved guest view afterwards so consumers that reconstruct/project positions from P and V separately see
    // the real eye camera. Their product remains exactly the same Pe * Ve used by the vertex path.
    if (!withView(mono, view, view, fov, nearUnits, out)) return false;
    std::memcpy(out.view, view, sizeof out.view);
    return true;
}

}  // namespace skyrecomp::vr

using skyrecomp::vr::StereoCameraHook;

extern "C" bool sky_vr_is_scene_camera(const float *ubo, uint64_t bytes) {
    return StereoCameraHook::isSceneCamera(ubo, static_cast<std::size_t>(bytes));
}

extern "C" bool sky_vr_eye_camera(const float mono[SKY_VR_CAMERA_FLOATS], const float reference[7],
                                  const float eye[7], const float fov[4], float units_per_metre,
                                  float out[SKY_VR_CAMERA_FLOATS]) {
    if (!mono || !reference || !eye || !fov || !out) return false;
    StereoCameraHook::CameraUbo in{}, result{};
    std::memcpy(&in, mono, sizeof in);
    const StereoCameraHook::Pose ref{{reference[0], reference[1], reference[2], reference[3]},
                                     {reference[4], reference[5], reference[6]}};
    const StereoCameraHook::Pose view{{eye[0], eye[1], eye[2], eye[3]}, {eye[4], eye[5], eye[6]}};
    const StereoCameraHook::Fov f{fov[0], fov[1], fov[2], fov[3]};
    if (!StereoCameraHook::eyeCamera(in, ref, view, f, units_per_metre, result)) return false;
    std::memcpy(out, &result, sizeof result);
    return true;
}

extern "C" bool sky_vr_is_scene_projection(const float *ubo, uint64_t bytes) {
    return StereoCameraHook::isSceneProjection(ubo, static_cast<std::size_t>(bytes));
}

extern "C" bool sky_vr_camera_with_view(const float mono[SKY_VR_CAMERA_FLOATS], const float game_view[12],
                                        const float view[12], const float fov[4], float near_units,
                                        float out[SKY_VR_CAMERA_FLOATS]) {
    if (!mono || !view || !fov || !out) return false;
    StereoCameraHook::CameraUbo in{}, result{};
    std::memcpy(&in, mono, sizeof in);
    const StereoCameraHook::Fov f{fov[0], fov[1], fov[2], fov[3]};
    if (!StereoCameraHook::withView(in, game_view ? game_view : in.view, view, f, near_units, result)) return false;
    std::memcpy(out, &result, sizeof result);
    return true;
}

extern "C" bool sky_vr_camera_direct(const float mono[SKY_VR_CAMERA_FLOATS], const float view[12],
                                      const float fov[4], float near_units, float out[SKY_VR_CAMERA_FLOATS]) {
    if (!mono || !view || !fov || !out) return false;
    StereoCameraHook::CameraUbo in{}, result{};
    std::memcpy(&in, mono, sizeof in);
    const StereoCameraHook::Fov f{fov[0], fov[1], fov[2], fov[3]};
    if (!StereoCameraHook::direct(in, view, f, near_units, result)) return false;
    std::memcpy(out, &result, sizeof result);
    return true;
}
