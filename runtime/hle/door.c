// The door Link stands at (Skyward Sword HD), for VR's physical door opening (runtime/vr/vr_input.c, vr_rig.c).
//
// A door's leaf has a world matrix, row-major 3x4 (rotation | translation), in the door object's heap block - kept in
// several copies, recomputed by the game every frame. Measured 2026-11-04 on Link's room's door: closed, the leaf's
// translation is the door's position on the floor (790.6, 0, -2183) and its axes are level (the y axis straight up);
// the game's door sequence turns it about ~60 degrees about the hinge. No pointer from Link's actor to the door was
// found, so the matrix is found by scanning the object heap when Link reaches a door (his action 0x6a, player.c) and
// followed from then on while it stays a level, orthonormal matrix near him.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

extern int player_action(void);
extern bool player_eye(float eye[3]);

#define HEAP_LO 0x7005000000ull
#define HEAP_HI 0x7008000000ull
#define DOOR_NEAR 160.0f  // (units, level: the door's origin from Link's eye point; measured ~60 at the door)

static uint64_t g_door;  // the matrix's address; 0: none
static float g_door_m[12];

static bool level_matrix(const float *m) {
    for (int i = 0; i < 12; i++)
        if (!isfinite(m[i]) || fabsf(m[i]) > 1e6f) return false;
    // orthonormal rows, the y axis up
    for (int r = 0; r < 3; r++) {
        const float *a = m + r * 4, *b = m + ((r + 1) % 3) * 4;
        if (fabsf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] - 1) > 2e-3f) return false;
        if (fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) > 2e-3f) return false;
    }
    return fabsf(m[5] - 1) < 2e-3f;
}

static bool near_link(const float *m, const float eye[3]) {
    const float dx = m[3] - eye[0], dz = m[11] - eye[2];
    return dx * dx + dz * dz < DOOR_NEAR * DOOR_NEAR && eye[1] - m[7] > 100 && eye[1] - m[7] < 220;
}

static uint64_t scan(const float eye[3]) {
    const uint64_t link = *(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x18299d0);  // (his own matrices follow him)
    uint64_t best = 0;
    float best_d = 1e30f;
    for (uint64_t a = HEAP_LO; a < HEAP_HI; a += 4) {
        const float *m = (const float *)(uintptr_t)a;
        // cheap tests first: translation near, then the rotation
        const float dx = m[3] - eye[0], dz = m[11] - eye[2];
        if (!(dx * dx + dz * dz < DOOR_NEAR * DOOR_NEAR)) continue;
        if (a >= link - 0x1000 && a < link + 0x8000) continue;
        if (dx * dx + dz * dz < 25.0f * 25.0f) continue;  // (at Link himself: his own)
        if (!near_link(m, eye) || !level_matrix(m)) continue;
        if (fabsf(m[0] - 1) < 1e-6f && fabsf(m[10] - 1) < 1e-6f) continue;  // (an identity-turned helper, not a leaf)
        const float d = dx * dx + dz * dz;
        if (d < best_d) best_d = d, best = a;
    }
    return best;
}

// (once per frame, vr_rig.c)
void door_track(void) {
    float eye[3];
    if (!player_eye(eye)) { g_door = 0; return; }
    if (g_door) {
        const float *m = (const float *)(uintptr_t)g_door;
        if (level_matrix(m) && near_link(m, eye)) memcpy(g_door_m, m, sizeof g_door_m);
        else g_door = 0;
    }
    static uint64_t next_scan;
    extern uint64_t renderer_present_count(void);
    const uint64_t now = renderer_present_count();
    if (!g_door && player_action() == 0x6a && now >= next_scan) {
        next_scan = now + 30;
        g_door = scan(eye);
        if (g_door) {
            memcpy(g_door_m, (const float *)(uintptr_t)g_door, sizeof g_door_m);
            LOG("[door] door at %.1f %.1f %.1f (matrix %#llx)\n", g_door_m[3], g_door_m[7], g_door_m[11],
                (unsigned long long)g_door);
        }
    }
}

// The leaf's world matrix (row-major 3x4) of the door Link is at. False: none.
bool door_matrix(float m[12]) {
    if (!g_door) return false;
    memcpy(m, g_door_m, sizeof g_door_m);
    return true;
}

// The handle (its knob) of the door Link is at, in the world, on his side of it. The door's frame (measured on his
// room's door): the leaf spans x -70 (the hinge, found from how the leaf turns) .. +70, its plane is z = 0, y up; the
// knob sits near the free edge at hand height.
#define HANDLE_X 55.0f
#define HANDLE_Y 90.0f
#define HANDLE_Z 7.0f
bool door_handle(float w[3]) {
    float m[12], eye[3];
    if (!door_matrix(m) || !player_eye(eye)) return false;
    const float d[3] = {eye[0] - m[3], eye[1] - m[7], eye[2] - m[11]};
    const float side = m[2] * d[0] + m[6] * d[1] + m[10] * d[2] < 0 ? -1.0f : 1.0f;  // (his z in the door's frame)
    const float l[3] = {HANDLE_X, HANDLE_Y, HANDLE_Z * side};
    for (int r = 0; r < 3; r++) w[r] = m[r * 4 + 0] * l[0] + m[r * 4 + 1] * l[1] + m[r * 4 + 2] * l[2] + m[r * 4 + 3];
    return true;
}
