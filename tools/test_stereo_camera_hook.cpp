// StereoCameraHook: the guest scene camera ABI and the per-eye camera math.
// clang++ -std=c++17 -Iruntime -Iruntime/vr tools/test_stereo_camera_hook.cpp runtime/nvn/vulkan/StereoCameraHook.cpp -o build/test_stereo_camera_hook
#include "nvn/vulkan/StereoCameraHook.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

using skyrecomp::vr::StereoCameraHook;

static int failures;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
    } while (0)
static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

// A camera as the game writes it: symmetric perspective (fy 1.2, 16:9), P[2][3] = 1, P[3][2] = -1, and a view that
// is a pure translation (camera at (10, 20, 30) looking down -Z).
static StereoCameraHook::CameraUbo game_camera(float y_sign = 1.0f) {
    StereoCameraHook::CameraUbo c{};
    const float fy = 1.2f, fx = fy / (16.0f / 9.0f);
    const float p[16] = {fx, 0, 0, 0,  0, y_sign * fy, 0, 0,  0, 0, 0.0001f, 1,  0, 0, -1, 0};
    const float v[12] = {1, 0, 0, -10,  0, 1, 0, -20,  0, 0, 1, -30};
    for (int i = 0; i < 16; i++) c.projection[i] = p[i];
    for (int i = 0; i < 12; i++) c.view[i] = v[i];
    return c;
}

// Clip-space x/w and y/w of an eye-space point under a row-major projection.
static void project(const float *p, float x, float y, float z, float *nx, float *ny) {
    const float cx = p[0] * x + p[1] * y + p[2] * z + p[3];
    const float cy = p[4] * x + p[5] * y + p[6] * z + p[7];
    const float cw = p[12] * x + p[13] * y + p[14] * z + p[15];
    *nx = cx / cw;
    *ny = cy / cw;
}

// A world point through the eye camera block (projection x view), to normalised device coordinates.
static void world_ndc(const StereoCameraHook::CameraUbo &c, float x, float y, float z, float *nx, float *ny) {
    const float *v = c.view;
    const float e[3] = {v[0] * x + v[1] * y + v[2] * z + v[3], v[4] * x + v[5] * y + v[6] * z + v[7],
                        v[8] * x + v[9] * y + v[10] * z + v[11]};
    project(c.projection, e[0], e[1], e[2], nx, ny);
}

int main() {
    static_assert(sizeof(StereoCameraHook::CameraUbo) == 112, "28 floats");
    static_assert(offsetof(StereoCameraHook::CameraUbo, view) == 64, "view follows the 4x4 projection");

    const StereoCameraHook::Pose head{};  // identity at the origin
    const StereoCameraHook::Fov fov{-0.9f, 0.8f, 0.85f, -0.95f};  // asymmetric, like a real headset eye
    StereoCameraHook::CameraUbo mono = game_camera(), out{};

    // ---- classification
    CHECK(StereoCameraHook::isSceneCamera(reinterpret_cast<const float *>(&mono), sizeof mono));
    CHECK(!StereoCameraHook::isSceneCamera(reinterpret_cast<const float *>(&mono), sizeof mono - 4));
    StereoCameraHook::CameraUbo ortho = mono;
    ortho.projection[14] = 0; ortho.projection[15] = 1;  // orthographic: w = 1
    CHECK(!StereoCameraHook::isSceneCamera(reinterpret_cast<const float *>(&ortho), sizeof ortho));
    StereoCameraHook::CameraUbo skewed = mono;
    skewed.view[0] = 2;  // not a rotation
    CHECK(!StereoCameraHook::isSceneCamera(reinterpret_cast<const float *>(&skewed), sizeof skewed));

    // ---- the eye at the reference pose: the game's view kept, the headset frustum maps onto [-1, 1]
    // (the game camera stands at (10, 20, 30) facing -z: world = camera-space point + (10, 20, 30))
    CHECK(StereoCameraHook::eyeCamera(mono, head, head, fov, 100.0f, out));
    for (int i = 0; i < 12; i++) CHECK(near(out.view[i], mono.view[i]));  // (the view always stays the game's)
    for (int i : {8, 9, 10, 11, 12, 13, 14, 15}) CHECK(near(out.projection[i], mono.projection[i]));  // depth rows kept
    float nx, ny;
    const float z = -5;  // (5 units in front)
    world_ndc(out, 10 + std::tan(fov.right) * -z, 20, 30 + z, &nx, &ny);
    CHECK(near(nx, 1));
    world_ndc(out, 10 + std::tan(fov.left) * -z, 20, 30 + z, &nx, &ny);
    CHECK(near(nx, -1));
    world_ndc(out, 10, 20 + std::tan(fov.up) * -z, 30 + z, &nx, &ny);
    CHECK(near(ny, 1));
    world_ndc(out, 10, 20 + std::tan(fov.down) * -z, 30 + z, &nx, &ny);
    CHECK(near(ny, -1));
    const float cx = -(std::tan(fov.right) + std::tan(fov.left)) / (std::tan(fov.right) - std::tan(fov.left));
    const float cy = -(std::tan(fov.up) + std::tan(fov.down)) / (std::tan(fov.up) - std::tan(fov.down));
    // (straight ahead of the eye lands at (cx, cy): the asymmetric frustum's centre offset)

    // ---- a game with a flipped Y keeps its orientation (and triangle winding)
    StereoCameraHook::CameraUbo flipped = game_camera(-1.0f);
    CHECK(StereoCameraHook::eyeCamera(flipped, head, head, fov, 100.0f, out));
    world_ndc(out, 10, 20 + std::tan(fov.up) * -z, 30 + z, &nx, &ny);
    CHECK(near(ny, -1));

    // ---- interpupillary offset: the right eye 32 mm to the right, 100 units per metre: a point 3.2 units right
    // of the game camera is straight ahead of it
    StereoCameraHook::Pose right_eye{};
    right_eye.position = {0.032f, 0, 0};
    CHECK(StereoCameraHook::eyeCamera(mono, head, right_eye, fov, 100.0f, out));
    world_ndc(out, 13.2f, 20, 25, &nx, &ny);
    CHECK(near(nx, cx) && near(ny, cy));

    // ---- characters: their vertices arrive in game-camera space (bone matrices premultiplied by the game's view)
    // and only meet the projection; they must land where the same world point does
    {
        const float pc[3] = {13.2f - 10, 0, 25 - 30};  // (the point above, in game-camera space)
        const float *p = out.projection;
        const float w = p[12] * pc[0] + p[13] * pc[1] + p[14] * pc[2] + p[15];
        CHECK(near((p[0] * pc[0] + p[1] * pc[1] + p[2] * pc[2] + p[3]) / w, cx));
    }

    // ---- the offset is taken in the reference's own axes: head turned 90 degrees left (about +Y), the eye moved
    // along world -Z is a move along the head's +X (10 units right of the game camera)
    const float s = std::sqrt(0.5f);
    StereoCameraHook::Pose turned{};
    turned.orientation = {0, s, 0, s};
    StereoCameraHook::Pose turned_eye = turned;
    turned_eye.position = {0, 0, -0.1f};
    CHECK(StereoCameraHook::eyeCamera(mono, turned, turned_eye, fov, 100.0f, out));
    world_ndc(out, 20, 20, 25, &nx, &ny);
    CHECK(near(nx, cx, 1e-3f) && near(ny, cy, 1e-3f));

    // ---- head rotation relative to the reference: looking 90 degrees left, the game's -X axis is straight ahead
    StereoCameraHook::Pose look_left{};
    look_left.orientation = {0, s, 0, s};
    CHECK(StereoCameraHook::eyeCamera(mono, head, look_left, fov, 100.0f, out));
    world_ndc(out, 5, 20, 30, &nx, &ny);  // (5 units to the camera's left)
    CHECK(near(nx, cx, 1e-3f) && near(ny, cy, 1e-3f));

    // ---- rejected input leaves the output alone
    StereoCameraHook::CameraUbo untouched = game_camera();
    CHECK(!StereoCameraHook::eyeCamera(ortho, head, head, fov, 100.0f, untouched));
    CHECK(!StereoCameraHook::eyeCamera(mono, head, head, StereoCameraHook::Fov{0.5f, -0.5f, 0.5f, -0.5f}, 100.0f, untouched));
    StereoCameraHook::Pose bad{};
    bad.orientation = {0, 0, 0, 0};
    CHECK(!StereoCameraHook::eyeCamera(mono, head, bad, fov, 100.0f, untouched));
    CHECK(!StereoCameraHook::eyeCamera(mono, head, head, fov, NAN, untouched));
    CHECK(near(untouched.view[3], -10));

    // ---- the C bridges (runtime/vr/vr.c, runtime/vr/vr_rig.c) match
    const float ref7[7] = {0, 0, 0, 1, 0, 0, 0}, eye7[7] = {0, 0, 0, 1, 0.032f, 0, 0};
    const float fov4[4] = {fov.left, fov.right, fov.up, fov.down};
    StereoCameraHook::CameraUbo c_out{};
    CHECK(sky_vr_eye_camera(reinterpret_cast<const float *>(&mono), ref7, eye7, fov4, 100.0f, reinterpret_cast<float *>(&c_out)));
    world_ndc(c_out, 13.2f, 20, 25, &nx, &ny);
    CHECK(near(nx, cx) && near(ny, cy));
    // a complete eye view: the eye 50 units behind the game camera; the near plane pushed to 15 units
    const float back_view[12] = {1, 0, 0, -10, 0, 1, 0, -20, 0, 0, 1, -80};
    CHECK(sky_vr_camera_with_view(reinterpret_cast<const float *>(&mono), nullptr, back_view, fov4, 15.0f, reinterpret_cast<float *>(&c_out)));
    world_ndc(c_out, 10, 20, 30, &nx, &ny);  // (the game camera's position: 50 ahead of the eye, centred)
    CHECK(near(nx, cx) && near(ny, cy));
    {   // depth: the far side of the depth range at 15 units in front of the eye (game near: 1 unit)
        const float pv[3] = {0, 0, -15};  // eye space
        float pc[3];  // -> game-camera space: the eye is 50 behind it
        pc[0] = pv[0]; pc[1] = pv[1]; pc[2] = pv[2] + 50;  // (eye space = camera space - 50 along z)
        const float *p = c_out.projection;
        const float zc = p[8] * pc[0] + p[9] * pc[1] + p[10] * pc[2] + p[11];
        const float wc = p[12] * pc[0] + p[13] * pc[1] + p[14] * pc[2] + p[15];
        CHECK(near(zc / wc, 1.0f - 15.0f * mono.projection[10], 1e-4f));  // (the near side of the depth range)
    }
    {   // fragment effects inspect projection and view separately: direct form stores the actual eye view while
        // producing the same projected world positions as the vertex-compatible folded form above
        StereoCameraHook::CameraUbo direct{};
        CHECK(sky_vr_camera_direct(reinterpret_cast<const float *>(&mono), back_view, fov4, 15.0f,
                                   reinterpret_cast<float *>(&direct)));
        for (int i = 0; i < 12; i++) CHECK(near(direct.view[i], back_view[i]));
        float dx, dy;
        world_ndc(direct, 10, 20, 30, &dx, &dy);
        world_ndc(c_out, 10, 20, 30, &nx, &ny);
        CHECK(near(dx, nx) && near(dy, ny));
    }
    {   // a block whose view is the camera times a model matrix (a terrain chunk with its own origin, scaled x2):
        // given the game camera, its local points land where the same world points do
        const float model[12] = {2, 0, 0, 100, 0, 2, 0, 5, 0, 0, 2, -40};
        StereoCameraHook::CameraUbo chunk = mono, chunk_out{};
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) {
                float v = c == 3 ? mono.view[r * 4 + 3] : 0.0f;
                for (int k = 0; k < 3; k++) v += mono.view[r * 4 + k] * model[k * 4 + c];
                chunk.view[r * 4 + c] = v;
            }
        CHECK(sky_vr_is_scene_projection(reinterpret_cast<const float *>(&chunk), sizeof chunk));
        CHECK(!sky_vr_is_scene_camera(reinterpret_cast<const float *>(&chunk), sizeof chunk));  // (scaled)
        CHECK(sky_vr_camera_with_view(reinterpret_cast<const float *>(&chunk), mono.view, back_view, fov4, 15.0f,
                                      reinterpret_cast<float *>(&chunk_out)));
        float lx, ly;
        world_ndc(chunk_out, -40, 7, 30, &lx, &ly);           // local (-40, 7, 30) = world (20, 19, 20)
        world_ndc(c_out, 20, 19, 20, &nx, &ny);
        CHECK(near(lx, nx, 1e-4f) && near(ly, ny, 1e-4f));
        CHECK(near(chunk_out.view[3], chunk.view[3]));       // (the block's view is kept)
    }

    if (failures) return EXIT_FAILURE;
    std::printf("StereoCameraHook: ABI, classification, frustum, IPD, reference axes, rotation, rejection OK\n");
    return EXIT_SUCCESS;
}
