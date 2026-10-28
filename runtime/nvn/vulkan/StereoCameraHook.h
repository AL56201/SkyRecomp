// Per-eye camera for Skyward Sword's scene constant buffer (vertex stage, uniform slot 0, 864 bytes).
//
// Guest ABI, 112 bytes / 28 floats, both matrices row-major and applied to column vectors:
//   [ 0..15] projection P, 4x4. OpenGL-style perspective: w_clip = -z_eye (P[3][2] = -1), P[2][3] = 1.
//   [16..27] view V, 3x4 affine (rows of [R | t]); eye space is right-handed, -Z forward, +Y up.
//
// The hook is pure: it reads the game's mono camera and returns an eye camera. It never writes guest memory, so
// the CPU-visible game state is identical with VR on or off. The renderer places the result into the
// transient copy of the constant buffer that the eye's draw binds.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SKY_VR_CAMERA_FLOATS 28

#ifdef __cplusplus
extern "C" {
#endif

// true: `ubo` (at least `bytes` long) holds a perspective scene camera, not a 2D / layout block.
bool sky_vr_is_scene_camera(const float *ubo, uint64_t bytes);

// Eye camera from the game's mono camera. Poses are {qx, qy, qz, qw, px, py, pz} in the OpenXR local space
// (metres); `reference` is the head pose the game camera stands for. fov is {left, right, up, down} in radians
// (OpenXR XrFovf). units_per_metre scales headset translation into game units. False: input rejected, out untouched.
bool sky_vr_eye_camera(const float mono[SKY_VR_CAMERA_FLOATS], const float reference[7], const float eye[7],
                       const float fov[4], float units_per_metre, float out[SKY_VR_CAMERA_FLOATS]);

// true: the block's projection (floats 0..15) is the game's perspective scene projection. Its view may be the
// camera's view or the camera's view times a model matrix (scaled, rotated, translated).
bool sky_vr_is_scene_projection(const float *ubo, uint64_t bytes);

// The eye camera for a complete eye view (row-major 3x4, world -> eye; runtime/vr_rig.c builds it), folded into
// the projection as P' = Pe * Ve * Vg^-1 with Vg = game_view, the game camera's own view (NULL: the block's view,
// when the block is the camera itself). The block's view is kept. The headset frustum as above. near_units > the
// game's near distance moves the near plane out to it (1st person: Link's head inside it is clipped).
bool sky_vr_camera_with_view(const float mono[SKY_VR_CAMERA_FLOATS], const float game_view[12], const float view[12],
                             const float fov[4], float near_units, float out[SKY_VR_CAMERA_FLOATS]);

// A conventional camera block with Pe in projection and Ve in an orthonormal view. Fragment effects such as
// screen-space water inspect the matrices separately and cannot use the vertex path's equivalent Pe * Ve * Vg^-1
// encoding. Light/projector matrices are rejected so projected shadow/decal bounds remain in their native space.
bool sky_vr_camera_direct(const float mono[SKY_VR_CAMERA_FLOATS], const float view[12], const float fov[4],
                          float near_units, float out[SKY_VR_CAMERA_FLOATS]);

#ifdef __cplusplus
}

#include <array>

namespace skyrecomp::vr {

class StereoCameraHook final {
public:
    struct Pose {
        std::array<float, 4> orientation{0, 0, 0, 1};  // x, y, z, w
        std::array<float, 3> position{};                // metres
    };
    struct Fov { float left{}, right{}, up{}, down{}; };  // radians, OpenXR sign convention
    struct alignas(16) CameraUbo {
        float projection[16];  // row-major 4x4
        float view[12];        // row-major 3x4
    };

    [[nodiscard]] static bool isSceneProjection(const float *ubo, std::size_t bytes) noexcept;
    [[nodiscard]] static bool isSceneCamera(const float *ubo, std::size_t bytes) noexcept;
    [[nodiscard]] static bool withView(const CameraUbo &mono, const float gameView[12], const float view[12],
                                       const Fov &fov, float nearUnits, CameraUbo &out) noexcept;
    [[nodiscard]] static bool direct(const CameraUbo &mono, const float view[12], const Fov &fov,
                                     float nearUnits, CameraUbo &out) noexcept;
    [[nodiscard]] static bool eyeCamera(const CameraUbo &mono, const Pose &reference, const Pose &eye,
                                        const Fov &fov, float unitsPerMetre, CameraUbo &out) noexcept;
};

static_assert(sizeof(StereoCameraHook::CameraUbo) == SKY_VR_CAMERA_FLOATS * sizeof(float), "guest camera ABI");

}  // namespace skyrecomp::vr
#endif
