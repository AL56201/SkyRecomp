// VR camera rig math (runtime/vr/vr_rig.c): anchors, eye views, level horizon.
// clang -std=c11 -Iruntime -Iruntime/vr tools/test_vr_rig.c runtime/vr/vr_rig.c -o build/test_vr_rig -lm
#include "vr_rig.h"
#include "vr_rig_internal.h"
#include "vr_settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// ---- what vr_rig.c links against in the game
VRConfig g_vr_config = {.world_scale = 100.0f, .level_horizon = true};
bool player_eye(float eye[3]) { (void)eye; return false; }
bool player_feet_y(float *y) { (void)y; return false; }
bool sky_vr_camera_with_view(const float *m, const float *g, const float *v, const float *f, float n, float *o) {
    (void)m; (void)g; (void)v; (void)f; (void)n; (void)o; return false;
}
bool sky_vr_camera_direct(const float *m, const float *v, const float *f, float n, float *o) {
    (void)m; (void)v; (void)f; (void)n; (void)o; return false;
}
bool sky_vr_is_scene_camera(const float *u, uint64_t b) { (void)u; (void)b; return false; }
bool sky_vr_is_scene_projection(const float *u, uint64_t b) { return u && b >= 28 * sizeof(float); }
bool game_camera_view(float v[12]) { (void)v; return false; }
uint64_t renderer_present_count(void) { return 0; }

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
static bool near(float a, float b, float e) { return fabsf(a - b) < e; }
static const float DEG = 3.14159265f / 180.0f;

// world point through a view (row-major 3x4)
static void apply(const float v[12], const float p[3], float out[3]) {
    for (int r = 0; r < 3; r++) out[r] = v[r * 4] * p[0] + v[r * 4 + 1] * p[1] + v[r * 4 + 2] * p[2] + v[r * 4 + 3];
}
// OpenXR orientation: rotation about an axis
static void axis_angle(float q[4], float x, float y, float z, float degrees) {
    const float h = degrees * DEG * 0.5f;
    q[0] = x * sinf(h); q[1] = y * sinf(h); q[2] = z * sinf(h); q[3] = cosf(h);
}

int main(void) {
    const float ref[7] = {0, 0, 0, 1, 0, 1.6f, 0};  // head at 1.6 m, facing -z
    float eye[7] = {0, 0, 0, 1, 0, 1.6f, 0};
    float view[12], p[3];
    VrRigAnchor a;

    // ---- 3rd person: the eye at the reference reproduces the game camera; an IPD offset shifts it as before
    const float game[12] = {1, 0, 0, -10, 0, 1, 0, -20, 0, 0, 1, -30};  // camera at (10, 20, 30), facing -z
    vr_rig_anchor_camera(&a, game, false, 0, 0);
    CHECK(vr_rig_view(&a, ref, eye, 100, view));
    for (int i = 0; i < 12; i++) CHECK(near(view[i], game[i], 1e-4f));
    eye[4] = 0.032f;  // right eye: 32 mm right
    CHECK(vr_rig_view(&a, ref, eye, 100, view));
    CHECK(near(view[3], game[3] - 3.2f, 1e-3f) && near(view[7], game[7], 1e-3f) && near(view[11], game[11], 1e-3f));
    eye[4] = 0;

    // ---- the reference keeps only its yaw: a head tilted down at the start does not tilt the world
    float tilted[7] = {0, 0, 0, 1, 0, 1.6f, 0};
    axis_angle(tilted, 1, 0, 0, -20);
    CHECK(vr_rig_view(&a, tilted, eye, 100, view));
    for (int i = 0; i < 12; i++) CHECK(near(view[i], game[i], 1e-4f));

    // ---- level horizon: a camera pitched 30 degrees down keeps its yaw and position, loses the pitch
    float pitched[12];
    {
        const float c = cosf(-30 * DEG), s = sinf(-30 * DEG);  // camera rotation about x (camera -> world)
        const float rc[9] = {1, 0, 0, 0, c, -s, 0, s, c};
        const float pos[3] = {10, 20, 30};
        for (int r = 0; r < 3; r++) {  // view = [Rc^T | -Rc^T pos]
            for (int k = 0; k < 3; k++) pitched[r * 4 + k] = rc[k * 3 + r];
            pitched[r * 4 + 3] = -(rc[0 * 3 + r] * pos[0] + rc[1 * 3 + r] * pos[1] + rc[2 * 3 + r] * pos[2]);
        }
    }
    vr_rig_anchor_camera(&a, pitched, true, 0, 0);
    CHECK(vr_rig_view(&a, ref, eye, 100, view));
    for (int i = 0; i < 12; i++) CHECK(near(view[i], game[i], 1e-4f));  // (level = the unpitched camera)
    // offsets: 10 up, 50 back
    vr_rig_anchor_camera(&a, game, true, 10, 50);
    CHECK(near(a.origin[0], 10, 1e-4f) && near(a.origin[1], 30, 1e-4f) && near(a.origin[2], 80, 1e-4f));

    // ---- 1st person: anchored at a point facing +x (yaw 90): a point 50 ahead is straight in front of the eye
    const float anchor_pos[3] = {100, 150, 200};
    vr_rig_anchor_point(&a, anchor_pos, 90 * DEG);
    CHECK(vr_rig_view(&a, ref, eye, 100, view));
    apply(view, (const float[3]){150, 150, 200}, p);
    CHECK(near(p[0], 0, 1e-3f) && near(p[1], 0, 1e-3f) && near(p[2], -50, 1e-3f));
    // +z is then on the right (facing +x, y up: right = facing - 90 degrees)
    apply(view, (const float[3]){100, 150, 230}, p);
    CHECK(p[0] > 29 && near(p[2], 0, 1e-3f));
    // stepping 0.5 m forward in the room moves the eye 50 units along +x
    eye[6] = -0.5f;
    CHECK(vr_rig_view(&a, ref, eye, 100, view));
    apply(view, (const float[3]){150, 150, 200}, p);
    CHECK(near(p[2], 0, 1e-3f));
    eye[6] = 0;
    CHECK(near(vr_rig_camera_yaw(view), 90 * DEG, 1e-4f));

    // ---- yaw_only
    float q[4], y[4];
    axis_angle(q, 1, 0, 0, 35);  // pitch only
    vr_rig_yaw_only(y, q);
    CHECK(near(y[0], 0, 1e-5f) && near(y[1], 0, 1e-5f) && near(y[2], 0, 1e-5f) && near(fabsf(y[3]), 1, 1e-5f));

    // ---- Link draw identity: proximity may confirm a large player skeleton, never an item or rigid draw. Once
    // confirmed, the same mesh stays hidden through an animation that moves every bone away from the eye.
    g_rig.mode = 1;
    g_rig.have_game_view = true;
    g_rig.link[0] = g_rig.link[1] = g_rig.link[2] = 0;
    memset(g_rig.game_view, 0, sizeof g_rig.game_view);
    g_rig.game_view[0] = g_rig.game_view[5] = g_rig.game_view[10] = 1;
    float camera[28] = {0}, small[5 * 12] = {0}, body[26 * 12] = {0};
    unsigned char item_mesh[96] = {1}, body_mesh[192] = {2}, other_mesh[192] = {3};
    CHECK(!vr_rig_is_link_draw(camera, sizeof camera, small, sizeof small, 100, (uint64_t)(uintptr_t)item_mesh,
                               sizeof item_mesh));
    CHECK(!vr_rig_is_link_draw(camera, sizeof camera, NULL, 0, 100, (uint64_t)(uintptr_t)item_mesh, sizeof item_mesh));
    CHECK(vr_rig_is_link_draw(camera, sizeof camera, body, sizeof body, 1000, (uint64_t)(uintptr_t)body_mesh,
                              sizeof body_mesh));
    for (int i = 0; i < 26; i++) body[i * 12 + 3] = body[i * 12 + 7] = body[i * 12 + 11] = 1000;
    CHECK(vr_rig_is_link_draw(camera, sizeof camera, body, sizeof body, 1000, (uint64_t)(uintptr_t)body_mesh,
                              sizeof body_mesh));
    CHECK(!vr_rig_is_link_draw(camera, sizeof camera, body, sizeof body, 1000, (uint64_t)(uintptr_t)other_mesh,
                               sizeof other_mesh));

    if (failures) return EXIT_FAILURE;
    printf("vr_rig: 3rd person = game camera, IPD, yaw-only reference, level horizon, offsets, 1st person anchor, "
           "room-scale steps OK\n");
    return EXIT_SUCCESS;
}
