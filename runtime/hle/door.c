// The door Link stands at (Skyward Sword HD), for VR's physical door opening (runtime/vr/vr_input.c, vr_rig.c).
//
// A door's leaf has a world matrix, row-major 3x4 (rotation | translation), in the door object's heap block - kept in
// several copies, recomputed by the game every frame. Measured 2026-11-04 on Link's room's door: closed, the leaf's
// translation is the door's position on the floor (790.6, 0, -2183) and its axes are level (the y axis straight up);
// the game's door sequence turns it about ~60 degrees about the hinge. No pointer from Link's actor to the door was
// found, so the matrix is found by scanning the object heap when Link reaches a door (his action 0x6a, player.c) and
// followed from then on while it stays a level, orthonormal matrix near him.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

extern int player_action(void);
extern bool player_eye(float eye[3]);

#define HEAP_LO 0x7005000000ull
#define HEAP_HI 0x7008000000ull
#define DOOR_NEAR 160.0f  // (units, level: the door's origin from Link's eye point; measured ~60 at the door)

static uint64_t g_door;  // the matrix's address; 0: none
static uint64_t g_door_actor_found;  // (its door object, found with it - below)
// How far VR holds the door open (radians, about its vertical through its origin - the leaf's draw turns about it in
// the game's own sequence; measured -22.5 -> -82 degrees of its x axis). vr_input.c sets it; vr_rig.c draws it.
#include <stdatomic.h>
static _Atomic float g_open;
void door_set_open(float radians) { atomic_store(&g_open, radians); }
float door_open(void) { return atomic_load(&g_open); }
static float g_door_m[12];
// (the copy found is turned 180 degrees from the drawn leaf: its x and z axes are taken negated)
static bool g_door_flip;
static void take(const float *m) {
    memcpy(g_door_m, m, sizeof g_door_m);
    if (g_door_flip)
        for (int r = 0; r < 3; r++) g_door_m[r * 4 + 0] = -g_door_m[r * 4 + 0], g_door_m[r * 4 + 2] = -g_door_m[r * 4 + 2];
}

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
        {   // (not one inside a door object - its own matrix at +0x1f4 faces the other way: the leaf's draw matrix is
            // in a block of its own)
            bool in_door = false;
            for (uint64_t o = (a & ~7ull); o + 0x900 > a && o > HEAP_LO && !in_door; o -= 8)
                in_door = *(const uint64_t *)(uintptr_t)o == 0x7001600da0ull;
            if (in_door) continue;
        }
        const float d = dx * dx + dz * dz;
        if (d < best_d) best_d = d, best = a;
    }
    return best;
}

// (once per frame, vr_rig.c)
void door_test_open(void);
// The level's door objects (vtable DOOR_VTABLE, position at +0x144): found a slice of the heap per frame (a whole
// scan at once is a hitch in the headset), dropped once their vtable is gone (the room unloaded)
#define DOOR_VTABLE_ 0x7001600da0ull
#define DOOR_KEEP 500.0f  // (units: a door left open stays open until Link is this far from it)
#define DOOR_REACH 150.0f  // (units, level: Link's eye from the door's origin - ~60 standing at it)
static uint64_t g_doors[64];
static int g_ndoors;
static bool door_object_near(const float eye[3]) {
    static uint64_t cursor = HEAP_LO;
    const uint64_t end = cursor + (HEAP_HI - HEAP_LO) / 32;
    for (uint64_t a = cursor; a < end; a += 8)
        if (*(const uint64_t *)(uintptr_t)a == DOOR_VTABLE_) {
            bool known = false;
            for (int i = 0; i < g_ndoors && !known; i++) known = g_doors[i] == a;
            if (!known && g_ndoors < 64) g_doors[g_ndoors++] = a;
        }
    cursor = end >= HEAP_HI ? HEAP_LO : end;
    bool near = false;
    for (int i = 0; i < g_ndoors; i++) {
        if (*(const uint64_t *)(uintptr_t)g_doors[i] != DOOR_VTABLE_) { g_doors[i--] = g_doors[--g_ndoors]; continue; }
        const float *p = (const float *)(uintptr_t)(g_doors[i] + 0x144);
        const float dx = p[0] - eye[0], dz = p[2] - eye[2];
        near |= dx * dx + dz * dz < DOOR_REACH * DOOR_REACH && eye[1] - p[1] > 100 && eye[1] - p[1] < 220;
    }
    return near;
}

// Link is at the door (within reach of it, either side): the gesture's
bool door_at(void) {
    float eye[3];
    if (!g_door || !player_eye(eye)) return false;
    const float dx = g_door_m[3] - eye[0], dz = g_door_m[11] - eye[2];
    return dx * dx + dz * dz < DOOR_REACH * DOOR_REACH;
}

void door_track(void) {
    door_test_open();
    {   // (diagnostics) reaching / leaving a door
        static bool was;
        const bool at = door_at();
        if (at != was) {
            float e[3] = {0, 0, 0};
            player_eye(e);
            float h[3] = {0, 0, 0};
            extern bool door_handle(float w[3]);
            if (at) door_handle(h);
            LOG("[door] %s the door (eye %.1f %.1f, handle %.1f %.1f %.1f)\n", at ? "at" : "left", e[0], e[2], h[0], h[1],
                h[2]);
            was = at;
        }
    }
    float eye[3];
    if (!player_eye(eye)) { g_door = 0; return; }
    if (g_door) {
        const float *m = (const float *)(uintptr_t)g_door;
        const float kx = m[3] - eye[0], kz = m[11] - eye[2];  // (kept while Link is within DOOR_KEEP of it)
        if (level_matrix(m) && kx * kx + kz * kz < DOOR_KEEP * DOOR_KEEP) take(m);
        else g_door = 0, door_set_open(0);  // (Link far from it: the door left open shuts)
    }
    static uint64_t next_scan;
    extern uint64_t renderer_present_count(void);
    const uint64_t now = renderer_present_count();
    // (Link's +0x118 is not his action but the collision polygon he touches - 0x6a was one wall of his room - so a
    // door is found from the door objects instead: from either side, any door)
    if (!g_door && door_object_near(eye) && now >= next_scan) {
        next_scan = now + 30;
        g_door = scan(eye);
        if (g_door) {
            g_door_flip = false;
            take((const float *)(uintptr_t)g_door);
            extern uint64_t door_find_actor(const float m[12]);
            g_door_actor_found = door_find_actor(g_door_m);
            if (g_door_actor_found) {
                // (the door object faces the other way from the drawn leaf: its yaw, s16 at +0x132, minus 180 degrees,
                // is the leaf's - a copy turned the other way is turned back)
                const float yaw = *(const int16_t *)(uintptr_t)(g_door_actor_found + 0x132) * (float)(M_PI / 32768) - (float)M_PI;
                const float leaf = atan2f(-g_door_m[8], g_door_m[0]);
                float d = leaf - yaw;
                while (d > (float)M_PI) d -= 2 * (float)M_PI;
                while (d < -(float)M_PI) d += 2 * (float)M_PI;
                if (fabsf(d) > (float)(M_PI / 2)) g_door_flip = true, take((const float *)(uintptr_t)g_door);
            }
            LOG("[door] door at %.1f %.1f %.1f (matrix %#llx, x axis %.3f %.3f)\n", g_door_m[3], g_door_m[7], g_door_m[11],
                (unsigned long long)g_door, g_door_m[0], g_door_m[8]);
        }
    }
}

// (test) SKYRECOMP_DOOR_TEST_OPEN=<degrees>: the door Link is at, held open that far
void door_test_open(void) {
    static int on = -1;
    static float deg;
    if (on < 0) on = getenv("SKYRECOMP_DOOR_TEST_OPEN") != NULL, deg = on ? (float)atof(getenv("SKYRECOMP_DOOR_TEST_OPEN")) : 0;
    if (on) door_set_open(g_door ? deg * (float)(M_PI / 180) : 0.0f);
    static bool requested;  // (and the room beyond, once - as a pulled door does)
    void door_request_room(void);
    if (on && g_door && !requested) requested = true, door_request_room();
    static int step_at = -1;  // (SKYRECOMP_DOOR_TEST_STEP=<frames>: and Link stepped through that long after)
    if (step_at < 0) step_at = getenv("SKYRECOMP_DOOR_TEST_STEP") ? atoi(getenv("SKYRECOMP_DOOR_TEST_STEP")) : 0;
    static int since;
    void door_request_step(void);
    if (on && requested && step_at > 0 && ++since == step_at) door_request_step();
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
// (measured 2026-11-04: the controllers held on the knob in VR sat at x 42..62, y 115, z 17..21 from the door's
// side - the grip's centre, in the hand around the knob)
#define HANDLE_X 52.0f
#define DOOR_HINGE_X (-70.0f)  // (the leaf turns about its edge away from the knob)
float door_hinge_x(void) { return DOOR_HINGE_X; }
#define HANDLE_Y 115.0f
#define HANDLE_Z 19.0f
static bool handle_at(float out_z, float w[3]) {
    float m[12], eye[3];
    if (!door_matrix(m) || !player_eye(eye)) return false;
    const float d[3] = {eye[0] - m[3], eye[1] - m[7], eye[2] - m[11]};
    const float side = m[2] * d[0] + m[6] * d[1] + m[10] * d[2] < 0 ? -1.0f : 1.0f;  // (his z in the door's frame)
    // (turned with the leaf: Ry(open) in the door's frame - x' = c x + s z, z' = -s x + c z)
    // (about the hinge, x = DOOR_HINGE_X: l = h + Ry (l0 - h))
    const float o = door_open(), c = cosf(o), s = sinf(o), lx = HANDLE_X - DOOR_HINGE_X, lz = out_z * side;
    const float l[3] = {DOOR_HINGE_X + c * lx + s * lz, HANDLE_Y, -s * lx + c * lz};
    for (int r = 0; r < 3; r++) w[r] = m[r * 4 + 0] * l[0] + m[r * 4 + 1] * l[1] + m[r * 4 + 2] * l[2] + m[r * 4 + 3];
    return true;
}
bool door_handle(float w[3]) { return handle_at(HANDLE_Z, w); }
// A point in the world -> the door's frame (its closed frame: R^T (p - t)); false: no door
bool door_local(const float p[3], float l[3]) {
    float m[12];
    if (!door_matrix(m)) return false;
    const float d[3] = {p[0] - m[3], p[1] - m[7], p[2] - m[11]};
    for (int i = 0; i < 3; i++) l[i] = m[0 * 4 + i] * d[0] + m[1 * 4 + i] * d[1] + m[2 * 4 + i] * d[2];
    return true;
}
// The handle in the door's closed frame, on Link's side (x, z)
bool door_handle_local(float *x, float *z) {
    float m[12], eye[3];
    if (!door_matrix(m) || !player_eye(eye)) return false;
    const float d[3] = {eye[0] - m[3], eye[1] - m[7], eye[2] - m[11]};
    const float side = m[2] * d[0] + m[6] * d[1] + m[10] * d[2] < 0 ? -1.0f : 1.0f;
    *x = HANDLE_X, *z = HANDLE_Z * side;
    return true;
}
// Where the hand holding the handle is drawn: the grip's centre further out from the door, the hand's mesh clear of it
#define HANDLE_HAND_Z 30.0f
bool door_handle_hand(float w[3]) { return handle_at(HANDLE_HAND_Z, w); }

// ---- the room beyond the door (no door event). Measured 2026-11-04 (Link's room -> the academy hallway):
// a door object (vtable 0x7001600da0) keeps its position at +0x144 / +0x148 / +0x14c and the two rooms it joins at
// +0x7bc / +0x7bd (bytes; 1 and 0 there). The door event activates the far room with f_7000e0854c(rooms, to, from):
// rooms = *(0x7002bf9d90), its room objects at +0xf0 + 8 * index; it clears the room's inactive bits (+0x9f0, 0xa
// when inactive) and marks the rooms around it - the far room's objects are then built, as when the door opens.
#define DOOR_VTABLE 0x7001600da0ull
#define ROOMS_PTR 0x7002bf9d90ull
#define g_door_actor g_door_actor_found
static _Atomic int g_room_request;

// (several objects of that type can stand at one door: all kept; door_rooms takes the one joining an active room to
// an inactive one)
static uint64_t g_door_actors[8];
static int g_door_actor_count;
uint64_t door_find_actor(const float m[12]) {
    g_door_actor_count = 0;
    for (uint64_t a = HEAP_LO; a < HEAP_HI && g_door_actor_count < 8; a += 8) {
        if (*(const uint64_t *)(uintptr_t)a != DOOR_VTABLE) continue;
        const float *p = (const float *)(uintptr_t)(a + 0x144);
        // (same floor too: the door straight above, on the next floor, is another room's - y 600 over Link's room's)
        if (fabsf(p[0] - m[3]) < 5 && fabsf(p[2] - m[11]) < 5 && fabsf(p[1] - m[7]) < 50) {
            g_door_actors[g_door_actor_count++] = a;
            LOG("[door] door object %#llx at %.1f %.1f %.1f yaw %04x rooms %d %d\n", (unsigned long long)a, p[0], p[1], p[2],
                *(const uint16_t *)(uintptr_t)(a + 0x132), *(const uint8_t *)(uintptr_t)(a + 0x7bc),
                *(const uint8_t *)(uintptr_t)(a + 0x7bd));
        }
    }
    return g_door_actor_count ? g_door_actors[0] : 0;
}

static uint64_t room_object(int index) {
    const uint64_t rooms = *(const uint64_t *)(uintptr_t)ROOMS_PTR;
    if (rooms < 0x7000000000ull || rooms >= 0x7800000000ull || index < 0 || index > 63) return 0;
    const uint64_t r = *(const uint64_t *)(uintptr_t)(rooms + 0xf0 + 8 * (uint64_t)index);
    return r >= 0x7000000000ull && r < 0x7800000000ull ? r : 0;
}

// The door's far room (the one of its two that is inactive) and the near one. False: no door object, or both active.
bool door_rooms(int *to, int *from) {
    if (!g_door_actor) return false;
    for (int i = 0; i < g_door_actor_count; i++) {
        const uint8_t *d = (const uint8_t *)(uintptr_t)g_door_actors[i];
        const int a = d[0x7bd], b = d[0x7bc];
        const uint64_t ra = room_object(a), rb = room_object(b);
        if (!ra || !rb || a == b) continue;
        const bool a_off = *(const uint8_t *)(uintptr_t)(ra + 0x9f0) & 0x1e, b_off = *(const uint8_t *)(uintptr_t)(rb + 0x9f0) & 0x1e;
        if (a_off == b_off) continue;
        *to = a_off ? a : b, *from = a_off ? b : a;
        return true;
    }
    return false;
}

// (vr_input.c, any thread) Activate the room beyond the door: done on the game thread (door_room_tick).
void door_request_room(void) { atomic_store(&g_room_request, 1); }

// (hle/vr_world.c, the game thread, inside an object's update)
void door_room_tick(Cpu *c) {
    if (!atomic_exchange(&g_room_request, 0)) return;
    int to, from;
    const uint64_t rooms = *(const uint64_t *)(uintptr_t)ROOMS_PTR;
    if (!door_rooms(&to, &from) || !rooms) {
        const uint8_t *d = (const uint8_t *)(uintptr_t)g_door_actor;
        const uint64_t ra = d ? room_object(d[0x7bd]) : 0, rb = d ? room_object(d[0x7bc]) : 0;
        LOG("[door] room request: no far room to activate (door object %#llx rooms %d %d -> %#llx %#llx flags %x %x)\n",
            (unsigned long long)g_door_actor, d ? d[0x7bd] : -1, d ? d[0x7bc] : -1, (unsigned long long)ra, (unsigned long long)rb,
            ra ? *(const uint8_t *)(uintptr_t)(ra + 0x9f0) : 0xff, rb ? *(const uint8_t *)(uintptr_t)(rb + 0x9f0) : 0xff);
        return;
    }
    uint64_t saved[31];
    memcpy(saved, c->x, sizeof saved);
    const uint64_t sp = c->sp, pc = c->pc;
    c->x[0] = rooms, c->x[1] = (uint64_t)to, c->x[2] = (uint64_t)from;
    guest_call(c, 0x7000e0854cull);
    memcpy(c->x, saved, sizeof saved);
    c->sp = sp, c->pc = pc;
    LOG("[door] activated room %d beyond the door (from room %d)\n", to, from);
    extern void door_mark_activated(void);
    door_mark_activated();
}

// ---- stepping through (the door's collision stays where the closed leaf is: the game's own sequence walks Link
// through by animation). Link's actor keeps his position x / z at +0x148 / +0x150, with copies at +0x15c / +0x164 (the
// previous frame's) - measured as the ones that follow him (snapshot diff, his room -> the door).
static _Atomic int g_step_request;
void door_request_step(void) { atomic_store(&g_step_request, 1); }

// (game thread) Link moved through the door: to the far side of its plane, `dist` units beyond it, at the doorway's
// centre line (the door's origin is the doorway's middle - its leaf spans x -70 .. 70)
void door_step_tick(void) {
    // a glide, not a jump: Link is carried from where he stands to `dist` beyond the door's plane over GLIDE frames
    // (once per frame - this runs for every object), his wall check skipped meanwhile (the closed door's collision
    // would push him back)
    enum { GLIDE = 36 };
    static int glide;  // (frames left; 0: none)
    static float x0, z0, x1, z1;
    // (every float that held his x at the door, z 8 bytes on - snapshot search, and his body's at +0x2940; each one's
    // height is held as it was: the skipped check is his floor's too)
    static const int32_t copies[] = {-0x254, -0x44, 0x1c, 0x48, 0x148, 0x15c, 0x220, 0x234, 0x2d8, 0x1634, 0x2940 + 0x144, 0x2940 + 0x1bc};
    enum { NCOPIES = sizeof copies / sizeof *copies };
    static float ys[NCOPIES];
    extern uint64_t renderer_present_count(void);
    static uint64_t last_frame;
    const uint64_t frame = renderer_present_count();
    if (frame == last_frame) return;
    float m[12], eye[3];
    const uint64_t link = *(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x18299d0);
    if (link < 0x7000000000ull || link >= 0x7800000000ull) return;
    if (!glide && atomic_load(&g_step_request)) {
        if (!door_matrix(m) || !player_eye(eye)) return;
        atomic_store(&g_step_request, 0);
        const float d[3] = {eye[0] - m[3], eye[1] - m[7], eye[2] - m[11]};
        const float side = m[2] * d[0] + m[6] * d[1] + m[10] * d[2] < 0 ? -1.0f : 1.0f;  // (his z in the door's frame)
        const float dist = 90.0f;
        // the door's z axis in the world (its third column), to the far side, on the doorway's centre line
        x1 = m[3] - side * m[2] * dist, z1 = m[11] - side * m[10] * dist;
        const float *p = (const float *)(uintptr_t)(link + 0x148);
        x0 = p[0], z0 = p[2];
        for (unsigned i = 0; i < NCOPIES; i++) ys[i] = ((const float *)(uintptr_t)(link + (int64_t)copies[i]))[1];
        glide = GLIDE;
        LOG("[door] Link walks through: %.1f %.1f -> %.1f %.1f\n", x0, z0, x1, z1);
    }
    if (!glide) return;
    last_frame = frame;
    glide--;
    float t = 1.0f - (float)glide / GLIDE;
    t = t * t * (3 - 2 * t);  // (eased)
    const float x = x0 + (x1 - x0) * t, z = z0 + (z1 - z0) * t;
    // (every float that held his x at the door, z 8 bytes on - snapshot search, and his body's at +0x2940)
    for (unsigned i = 0; i < NCOPIES; i++) {
        float *q = (float *)(uintptr_t)(link + (int64_t)copies[i]);
        q[0] = x, q[1] = ys[i], q[2] = z;
    }
    extern volatile int g_fnprof, g_door_nowall;
    g_door_nowall = 3; g_fnprof = 1;  // (fnprof.c: his next checks skipped, the last one runs again)
}

// ---- the door object turned (its collision with it): its rotation, s16 x / y / z at +0x130 and a copy at +0x13c
// (y 0x9000 = 202.5 degrees for Link's room's door, whose leaf faces 22.5). (test) SKYRECOMP_DOOR_TEST_YAW=<degrees>:
// the door object at the door Link is at turned that much, every frame (game thread)
void door_yaw_tick(void) {
    static int on = -1;
    static float deg;
    if (on < 0) on = getenv("SKYRECOMP_DOOR_TEST_YAW") != NULL, deg = on ? (float)atof(getenv("SKYRECOMP_DOOR_TEST_YAW")) : 0;
    if (!on || !g_door_actor_count) return;
    int to, from;
    if (!door_rooms(&to, &from)) {}  // (keeps none: any door object at the door turns)
    static uint16_t base[8];
    static uint64_t based[8];
    for (int i = 0; i < g_door_actor_count; i++) {
        const uint64_t a = g_door_actors[i];
        uint16_t *y0 = (uint16_t *)(uintptr_t)(a + 0x132), *y1 = (uint16_t *)(uintptr_t)(a + 0x13e);
        if (based[i] != a) based[i] = a, base[i] = *y0;
        const uint16_t y = (uint16_t)(base[i] + (int)(deg * 65536.0f / 360.0f));
        *y0 = y, *y1 = y;
    }
}

// (test) SKYRECOMP_DOOR_TEST_POSE=<file of "hex address hex u32" lines>: written every object update - the leaf's
// matrices at the game's fully open pose, to see whether the door's collision follows them
void door_pose_tick(void) {
    static int n = -1;
    static uint64_t addr[256];
    static uint32_t val[256];
    if (n < 0) {
        n = 0;
        const char *p = getenv("SKYRECOMP_DOOR_TEST_POSE");
        FILE *f = p ? fopen(p, "r") : NULL;
        unsigned long long a;
        unsigned v;
        while (f && n < 256 && fscanf(f, "%llx %x", &a, &v) == 2) addr[n] = a, val[n] = v, n++;
        if (f) fclose(f);
        if (n) LOG("[door] test pose: %d values\n", n);
    }
    if (!n || !g_door_actor_count) return;
    for (int i = 0; i < n; i++) *(volatile uint32_t *)(uintptr_t)addr[i] = val[i];
}

// (test) SKYRECOMP_DOOR_TEST_LIFT=<units>: the door objects at the door lifted that far every object update (their
// position y, +0x148) - is Link's "at a door" stop (and the door's collision) theirs?
void door_lift_tick(void) {
    static int on = -1;
    static float up;
    if (on < 0) on = getenv("SKYRECOMP_DOOR_TEST_LIFT") != NULL, up = on ? (float)atof(getenv("SKYRECOMP_DOOR_TEST_LIFT")) : 0;
    if (!on) return;
    static float base[8];
    static uint64_t based[8];
    for (int i = 0; i < g_door_actor_count; i++) {
        float *y = (float *)(uintptr_t)(g_door_actors[i] + 0x148);
        if (based[i] != g_door_actors[i]) based[i] = g_door_actors[i], base[i] = *y;
        *y = base[i] + up;
    }
}

// (test) SKYRECOMP_DOOR_TEST_LINK=<hex offset>,<hex u32>: Link's actor field held at that value every object update
// once a door's room was activated - which of the fields the game's door sequence changes lets him walk through?
static bool g_room_activated;
void door_link_tick(void) {
    static int on = -1;
    static unsigned off, val;
    if (on < 0) on = getenv("SKYRECOMP_DOOR_TEST_LINK") && sscanf(getenv("SKYRECOMP_DOOR_TEST_LINK"), "%x,%x", &off, &val) == 2;
    if (!on || !g_room_activated) return;
    const uint64_t link = *(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x18299d0);
    if (link >= 0x7000000000ull && link < 0x7800000000ull) *(volatile uint32_t *)(uintptr_t)(link + off) = val;
}

void door_mark_activated(void) { g_room_activated = true; }
