// VR camera rig (see vr_rig.h): the eye cameras, Link's own draws hidden in 1st person, and culling.
#include "vr_rig_internal.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vr_settings.h"

bool player_eye(float eye[3]);  // runtime/hle/player.c: Link's eye point (feet + 150 units)
bool player_feet_y(float *y);   // runtime/hle/player.c: the world height of Link's feet
#if !defined(__APPLE__)
bool sky_vr_camera_with_view(const float mono[28], const float game_view[12], const float view[12], const float fov[4],
                             float near_units, float out[28]);  // runtime/nvn/vulkan/StereoCameraHook.h
bool sky_vr_camera_direct(const float mono[28], const float view[12], const float fov[4], float near_units,
                          float out[28]);
bool sky_vr_is_scene_camera(const float *ubo, uint64_t bytes);
bool sky_vr_is_scene_projection(const float *ubo, uint64_t bytes);
bool game_camera_view(float view[12]);  // runtime/hle/player.c: the game camera's own view
#endif

// 1st person: geometry nearer than this is clipped (Link's own head around the eyes). SKYRECOMP_VR_NEAR overrides.
#define FIRST_PERSON_NEAR 15.0f
// 1st person -> theatre (3rd-person view) when the game camera is farther than this from Link's eye point, back
// below THEATRE_LEAVE. Walking, the camera trails Link by ~400 units.
#define THEATRE_ENTER 1500.0f
#define THEATRE_LEAVE 1000.0f
#define THEATRE_FRAMES 20  // frames the camera must stay far before switching
// A skinned draw is Link's when one of its bones lies this close to his eye point (measured: a few units standing,
// up to ~100 swimming / jumping, where the eye point stays at floor + 150; other characters' nearest bones stayed
// 150+ away).
#define LINK_BONE_RADIUS 110.0f
// Link's skinned body parts use at least 26 matrices; held items use small palettes (the Slingshot has 5). Do not
// let an item become "Link" merely because it is brought close to the headset.
#define LINK_BODY_MIN_PALETTE 26
#define LINK_MESH_CACHE 64
// Swimming: the game keeps the eye point at the water surface (even while Link dives), which cuts the view in half.
// In water the eye point is more than SWIM_EYE_ABOVE_FEET above Link's feet (on land it is feet + 150, measured up to
// 151.2) and holds perfectly still (on stairs the gap can grow too, but the height keeps changing). Then, while Link
// himself is drawn there (not so in the knight's rescue, where the same numbers occur), the eye is kept at least
// SWIM_EYE_RAISE above the surface. This is a hard clearance while swimming: easing or room-scale head movement
// through the water plane makes its screen-space reflection singular (flat fans / streaks). The offset eases out.
// (Following his feet instead fails: swimming at the surface they sit 154..196 below it depending on his pose.)
#define SWIM_EYE_ABOVE_FEET 153.0f
#define SWIM_STILL_FRAMES 10
#define SWIM_EYE_RAISE 12.0f

// ================================================================== math
typedef struct { float x, y, z, w; } Quat;

static Quat q_mul(Quat a, Quat b) {
    return (Quat){a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                  a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static Quat q_conj(Quat q) { return (Quat){-q.x, -q.y, -q.z, q.w}; }
static Quat q_norm(Quat q) {
    float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-6f) || !isfinite(n)) return (Quat){0, 0, 0, 1};
    return (Quat){q.x / n, q.y / n, q.z / n, q.w / n};
}
static void q_matrix(Quat q, float m[9]) {  // row-major rotation
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                        2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                        2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
    memcpy(m, r, sizeof r);
}
static void yaw_matrix(float yaw, float m[9]) {  // rotation about +y taking +z to (sin yaw, 0, cos yaw)
    const float c = cosf(yaw), s = sinf(yaw);
    const float r[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
    memcpy(m, r, sizeof r);
}

void vr_rig_yaw_only(float out[4], const float orientation[4]) {
    Quat q = q_norm((Quat){orientation[0], orientation[1], orientation[2], orientation[3]});
    float m[9], f[3];
    q_matrix(q, m);
    m_vec(m, (const float[3]){0, 0, -1}, f);
    float yaw = (fabsf(f[0]) < 1e-5f && fabsf(f[2]) < 1e-5f) ? 0.0f : atan2f(-f[0], -f[2]);  // (OpenXR: about +y)
    out[0] = 0; out[1] = sinf(yaw * 0.5f); out[2] = 0; out[3] = cosf(yaw * 0.5f);
}

void vr_rig_anchor_camera(VrRigAnchor *a, const float v[12], bool level, float height, float distance) {
    // camera -> world rotation = R^T; camera position = -R^T t
    const float rc[9] = {v[0], v[4], v[8], v[1], v[5], v[9], v[2], v[6], v[10]};
    const float t[3] = {v[3], v[7], v[11]};
    float pos[3];
    m_vec(rc, t, pos);
    for (int i = 0; i < 3; i++) pos[i] = -pos[i];
    const float f[3] = {-v[8], -v[9], -v[10]};  // forward (-z) in the world
    const float n = sqrtf(f[0] * f[0] + f[2] * f[2]);
    if (level && n > 1e-4f) yaw_matrix(atan2f(-f[0], -f[2]), a->rotation);
    else memcpy(a->rotation, rc, sizeof rc);
    pos[1] += height;
    if (n > 1e-4f) { pos[0] -= f[0] / n * distance; pos[2] -= f[2] / n * distance; }
    memcpy(a->origin, pos, sizeof pos);
}

void vr_rig_anchor_point(VrRigAnchor *a, const float position[3], float yaw) {
    yaw_matrix(yaw + (float)M_PI, a->rotation);  // the anchor's -z faces `yaw`
    memcpy(a->origin, position, sizeof a->origin);
}

void vr_rig_relative(const float reference[7], const float eye[7], float units, float rot[9], float offset[3]) {
    float ry[4];
    vr_rig_yaw_only(ry, reference);
    const Quat ref = {ry[0], ry[1], ry[2], ry[3]};
    const Quat q = q_norm((Quat){eye[0], eye[1], eye[2], eye[3]});
    q_matrix(q_mul(q_conj(ref), q), rot);
    float inv[9], d[3] = {eye[4] - reference[4], eye[5] - reference[5], eye[6] - reference[6]};
    q_matrix(q_conj(ref), inv);
    m_vec(inv, d, offset);
    for (int i = 0; i < 3; i++) offset[i] *= units;
}

bool vr_rig_view(const VrRigAnchor *a, const float reference[7], const float eye[7], float units, float view[12]) {
    if (!(units > 0) || !isfinite(units)) return false;
    float rel[9], off[3], rw[9], p[3];
    vr_rig_relative(reference, eye, units, rel, off);
    m_mul(a->rotation, rel, rw);
    m_vec(a->rotation, off, p);
    for (int i = 0; i < 3; i++) p[i] += a->origin[i];
    for (int r = 0; r < 3; r++) {  // view = [Rw^T | -Rw^T p]
        view[r * 4 + 0] = rw[0 * 3 + r];
        view[r * 4 + 1] = rw[1 * 3 + r];
        view[r * 4 + 2] = rw[2 * 3 + r];
        view[r * 4 + 3] = -(rw[0 * 3 + r] * p[0] + rw[1 * 3 + r] * p[1] + rw[2 * 3 + r] * p[2]);
    }
    for (int i = 0; i < 12; i++)
        if (!isfinite(view[i])) return false;
    return true;
}

float vr_rig_camera_yaw(const float v[12]) { return atan2f(-v[8], -v[10]); }



// ================================================================== state
// Renderer thread: everything but the values the game thread reads (atomics).
VrRigShared g_rig;
static bool g_yaw_set;          // 1st person: g_yaw holds the anchor's facing
static float g_yaw;             // 1st person anchor facing (where the game camera looked on entry)
static float g_swim_raise;      // current eye height added to the eye point (swimming), eased
static bool g_water;            // this frame: the eye point / feet gap says Link is in water
static int g_water_hits_now, g_water_hits_before;  // Link's draws found at the eye point in water: this frame, the last
// The game camera from game memory is trusted while draws show it is live: some draw's view equalled it this frame
// or the previous one (in a cutscene the address might hold another camera than the one drawing).
static bool g_memory_seen_now, g_memory_seen_before;
// The memory camera matched a draw within the last MEMORY_TRUST_FRAMES frames. On the title screen it matches only
// every other frame or so (it holds the next camera while the slowly panning one is drawn); deciding per frame
// switched the eye cameras' and the culling's source every 2-3 frames, and islands / clouds flickered.
enum { MEMORY_TRUST_FRAMES = 60 };
static _Atomic uint64_t g_memory_seen_frame;  // (present count + 1; 0 never)
static bool memory_recent(void) {
    extern uint64_t renderer_present_count(void);
    const uint64_t f = atomic_load(&g_memory_seen_frame);
    return f && renderer_present_count() + 1 <= f + MEMORY_TRUST_FRAMES;
}

float vr_rig_units_per_metre(void) {
    static float env = -1;
    if (env < 0) {
        // SKYRECOMP_VR_SCALE now means internal render resolution. Preserve
        // compatibility with its former world-scale meaning only for values
        // in the documented units-per-metre range.
        const char *value = getenv("SKYRECOMP_VR_WORLD_SCALE");
        if (!value) value = getenv("SKYRECOMP_VR_SCALE");
        env = value ? (float)atof(value) : 0.0f;
        if (env < 10.0f || env > 1000.0f) env = 0.0f;
    }
    if (env > 0) return env;
    const float s = g_vr_config.world_scale;
    return s >= 10.0f && s <= 1000.0f ? s : 100.0f;
}

static float g_near_clip = FIRST_PERSON_NEAR;  // (VR Options > Near Clip: vr_rig_set_near_clip, from vr.c)
void vr_rig_set_near_clip(float units) {
    if (units >= 1.0f && units <= 100.0f) g_near_clip = units;
}

static float near_units(void) {  // (SKYRECOMP_VR_NEAR overrides the setting)
    static float env = -2;
    if (env < -1) env = getenv("SKYRECOMP_VR_NEAR") ? (float)atof(getenv("SKYRECOMP_VR_NEAR")) : -1;
    return env >= 0 ? env : g_near_clip;
}

void vr_rig_recenter(void) { g_yaw_set = false; }

void vr_rig_toggle_view(void) {
    g_vr_config.view_mode = g_vr_config.view_mode ? 0 : 1;
    g_yaw_set = false;
}

void vr_rig_begin_frame(void) {
    g_water_hits_before = g_water_hits_now;
    g_water_hits_now = 0;
    g_memory_seen_before = g_memory_seen_now;
    g_memory_seen_now = false;
    int mode = 0;
    float pos[3];
    // Theatre: 1st person drops to the 3rd-person view while the game camera is far from Link's eye point. Link's
    // eye point stops updating while he rides his Loftwing, and cinematic cameras frame scenes from far away; the
    // view comes back once the camera follows Link again. Hysteresis keeps it from flickering at the edge.
    static bool theatre;
    float camera_distance = -1;
    const bool have_eye = g_vr_config.view_mode == 1 && player_eye(pos);
    if (have_eye && g_rig.have_game_view) {
        const float *v = g_rig.game_view;
        float d2 = 0;
        for (int i = 0; i < 3; i++) {
            const float c = -(v[i] * v[3] + v[4 + i] * v[7] + v[8 + i] * v[11]);  // camera position = -R^T t
            d2 += (c - pos[i]) * (c - pos[i]);
        }
        camera_distance = sqrtf(d2);
        static int far_frames;  // (a camera cut for a frame or two doesn't switch the view)
        far_frames = camera_distance > THEATRE_ENTER ? far_frames + 1 : 0;
        if (far_frames >= THEATRE_FRAMES) theatre = true;
        else if (camera_distance < THEATRE_LEAVE) theatre = false;
    }
    if (!have_eye) theatre = false;
    if (have_eye && !theatre) {
        // (entering 1st person: facing where the game camera looked, i.e. where you were already looking)
        if (!g_yaw_set) { g_yaw = g_rig.have_game_view ? vr_rig_camera_yaw(g_rig.game_view) : 0.0f; g_yaw_set = true; }
        static int follow = -1;  // (test) SKYRECOMP_VR_FP_FOLLOW_CAMERA=1: faces the game camera's yaw every frame
        if (follow < 0) follow = getenv("SKYRECOMP_VR_FP_FOLLOW_CAMERA") != NULL;
        if (follow && g_rig.have_game_view) g_yaw = vr_rig_camera_yaw(g_rig.game_view);
        memcpy(g_rig.link, pos, sizeof pos);
        mode = 1;
        static float last_eye_y;
        static int still;
        still = pos[1] == last_eye_y ? still + 1 : 0;
        last_eye_y = pos[1];
        float feet = 0;
        g_water = still >= SWIM_STILL_FRAMES && player_feet_y(&feet) && pos[1] - feet > SWIM_EYE_ABOVE_FEET;
        const bool swimming = g_water && g_water_hits_before > 0;
        // Do not ease towards the water clearance: the intermediate positions are precisely where the reflection
        // breaks. Leaving the water can stay smooth because every intermediate point is already above its plane.
        if (swimming) g_swim_raise = SWIM_EYE_RAISE;
        else g_swim_raise += (0.0f - g_swim_raise) * 0.15f;
    }
    if (mode != 1) { g_yaw_set = false; g_swim_raise = 0; g_water = false; }  // (re-entering 1st person faces Link's direction again)
    // SKYRECOMP_VR_CAM_TRACE=1: when the memory camera starts / stops matching the draws, and the rig mode changes
    static int trace = -1, last_seen = -1, last_mode = -1;
    if (trace < 0) trace = getenv("SKYRECOMP_VR_CAM_TRACE") != NULL;
    extern uint64_t renderer_present_count(void);
    if (trace && camera_distance >= 0 && renderer_present_count() % 120 == 0) {
        float feet = NAN;
        player_feet_y(&feet);
        fprintf(stderr, "[vr-rig] frame %llu: game camera %.0f units from Link's eye; eye %.1f above his feet, eye raised %.1f\n",
                (unsigned long long)renderer_present_count(), camera_distance, pos[1] - feet, g_swim_raise);
    }
    if (trace && (last_seen != g_memory_seen_before || last_mode != mode)) {
        fprintf(stderr, "[vr-rig] frame %llu: memory camera %s, %s person%s\n", (unsigned long long)renderer_present_count(),
                g_memory_seen_before ? "matches the draws" : "NOT seen in the draws (fallback)", mode ? "1st" : "3rd",
                theatre ? " (theatre: game camera far from Link)" : "");
        last_seen = g_memory_seen_before;
        last_mode = mode;
    }
    g_rig.mode = mode;
}

void vr_rig_current_anchor(VrRigAnchor *a, const float *game_view) {
    if (g_rig.mode == 1) {
        const float eye[3] = {g_rig.link[0], g_rig.link[1] + g_vr_config.camera_height_offset + g_swim_raise, g_rig.link[2]};
        vr_rig_anchor_point(a, eye, g_yaw);
    } else {
        vr_rig_anchor_camera(a, game_view, g_vr_config.level_horizon, g_vr_config.camera_height_offset,
                             g_vr_config.camera_distance_offset);
    }
}

static bool same_view(const float a[12], const float b[12]) {
    for (int i = 0; i < 12; i++)
        if (fabsf(a[i] - b[i]) > ((i & 3) == 3 ? 1e-3f * (1.0f + fabsf(a[i])) : 1e-5f)) return false;
    return true;
}

static float det3(const float v[12]) {  // the 3x3 part of a row-major 3x4
    return v[0] * (v[5] * v[10] - v[6] * v[9]) - v[1] * (v[4] * v[10] - v[6] * v[8]) + v[2] * (v[4] * v[9] - v[5] * v[8]);
}

// How far a model matrix's 3x3 is from upright (its y axis the world's): the game places objects with yaw and
// scale only, so the model matrix that explains a block's view should be upright.
uint64_t g_vr_pass_id, g_vr_pass_epoch;  // (exec.c, CMD_SET_RENDER_TARGETS)

static float tilt(const float m[9]) {
    float s = 0;
    for (int i = 0; i < 9; i++) s = fmaxf(s, fabsf(m[i]));
    if (!(s > 0)) return 1e9f;
    return (fabsf(m[1]) + fabsf(m[3]) + fabsf(m[5]) + fabsf(m[7])) / s;
}

// A planar reflection block (mirror floors, lakes): view Vm = F Vg D M - the game camera Vg reflected in a
// horizontal plane (D = diag(1,-1,1) on world axes), the image turned upside down (F, the same on view axes), times
// a model matrix M. A normal block is Vm = Vg M. Whichever reading gives the upright M wins.
static bool planar_reflection(const float v[12], const float g[12]) {
    static int off = -1;  // SKYRECOMP_VR_NO_REFLECTION_FIX=1: the earlier behaviour
    if (off < 0) off = getenv("SKYRECOMP_VR_NO_REFLECTION_FIX") != NULL;
    if (off) return false;
    float plain[9], mirror[9];  // Vg^-1 Vm and D Vg^-1 F Vm (rotation parts; Vg^-1 = Vg^T)
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            float p = 0, m = 0;
            for (int k = 0; k < 3; k++) {
                p += g[k * 4 + r] * v[k * 4 + c];
                m += g[k * 4 + r] * v[k * 4 + c] * (k == 1 ? -1.0f : 1.0f);
            }
            plain[r * 3 + c] = p;
            mirror[r * 3 + c] = m * (r == 1 ? -1.0f : 1.0f);
        }
    const float tp = tilt(plain), tm = tilt(mirror);
    // A block that tells (one of the readings upright, the other not) decides for its whole pass: the draws whose
    // model matrix isn't upright (or a level camera, where both readings agree) follow it. The pass's targets
    // remember the answer for the next frame, for the draws before the first deciding one.
    enum { KNOWN = 16 };
    static uint64_t known_id[KNOWN];
    static int8_t known_refl[KNOWN];
    static int next_slot;
    static uint64_t epoch = ~0ull;
    static int state;  // this pass: -1 plain, 1 reflection, 0 not decided
    int slot = -1;
    for (int i = 0; i < KNOWN; i++)
        if (known_id[i] == g_vr_pass_id) slot = i;
    if (epoch != g_vr_pass_epoch) { epoch = g_vr_pass_epoch; state = slot >= 0 ? known_refl[slot] : 0; }
    const int decided = tm < 0.02f && tp > 0.05f && tm < 0.25f * tp ? 1 : tp < 0.02f && tm > 0.05f && tp < 0.25f * tm ? -1 : 0;
    if (decided && decided != state) {
        state = decided;
        if (slot < 0 && decided > 0) slot = next_slot, next_slot = (next_slot + 1) % KNOWN, known_id[slot] = g_vr_pass_id;
        if (slot >= 0) known_refl[slot] = (int8_t)decided;
    }
    return state > 0;
}

static bool eye_camera(const float mono[28], const float reference[7], const float eye[7], const float fov[4],
                       float out[28], bool direct) {
#if defined(__APPLE__)
    (void)mono; (void)reference; (void)eye; (void)fov; (void)out;
    return false;
#else
    if (!mono || !sky_vr_is_scene_projection(mono, 28 * sizeof(float))) return false;
    // The game camera Vg. A block's view is Vg times the draw's model matrix (terrain chunks, the sky dome, Link's
    // skinned parts), so Vg comes from game memory; mirrored blocks (a reflection pass: its own camera) and frames
    // where memory doesn't match any draw fall back to the block's own view when that is a plain camera.
    float camera[12];
    const float *game;
    int source;
    static int no_memory = -1;  // SKYRECOMP_VR_NO_GAME_CAMERA=1: only the block's own view (the earlier behaviour)
    if (no_memory < 0) no_memory = getenv("SKYRECOMP_VR_NO_GAME_CAMERA") != NULL;
    const bool memory = !no_memory && game_camera_view(camera);
    if (memory && same_view(camera, mono + 16)) {
        extern uint64_t renderer_present_count(void);
        g_memory_seen_now = true;
        atomic_store(&g_memory_seen_frame, renderer_present_count() + 1);
    }
    // Without the memory camera (the title screen, file select, cutscenes: another camera draws), a plain camera
    // block of the same render pass (its targets; this frame or kept from an earlier one) stands for Vg in that
    // pass's blocks that are Vg times a model matrix: otherwise those (the sky dome, the cloud ring and sea) kept
    // the mono camera, stayed fixed to the head, and flickered whenever the memory camera matched for a frame.
    // (Other passes draw with other cameras: reflections, glare.)
    enum { PASS_CAMERAS = 16 };
    static struct { uint64_t pass; float view[12]; } pass_camera[PASS_CAMERAS];
    static int pass_camera_next;
    int pc = -1;
    for (int i = 0; i < PASS_CAMERAS; i++)
        if (pass_camera[i].pass == g_vr_pass_id) pc = i;
    if (memory && memory_recent() && det3(mono + 16) > 0) game = camera, source = 0;
    else if (sky_vr_is_scene_camera(mono, 28 * sizeof(float))) {
        if (!direct && pc >= 0 && fabsf(mono[5] - 5.0f) < .01f &&
            getenv("SKYRECOMP_VR_MODEL_CAMERA_TRACE") && !same_view(pass_camera[pc].view, mono + 16)) {
            extern uint64_t renderer_present_count(void);
            static uint64_t logged_frame = ~0ull;
            static int logged;
            const uint64_t frame_no = renderer_present_count();
            if (logged_frame != frame_no) { logged_frame = frame_no; logged = 0; }
            if (logged++ < 60) {
                fprintf(stderr, "[vr-model-camera] frame %llu pass %llu prior", (unsigned long long)frame_no,
                        (unsigned long long)g_vr_pass_id);
                for (int j = 0; j < 12; j++) fprintf(stderr, " %.8g", pass_camera[pc].view[j]);
                fprintf(stderr, " next");
                for (int j = 0; j < 12; j++) fprintf(stderr, " %.8g", mono[16 + j]);
                fprintf(stderr, "\n");
            }
        }
        game = mono + 16, source = 1;
        if (det3(mono + 16) > 0 && g_vr_pass_id) {
            if (pc < 0) pc = pass_camera_next, pass_camera_next = (pass_camera_next + 1) % PASS_CAMERAS;
            pass_camera[pc].pass = g_vr_pass_id;
            memcpy(pass_camera[pc].view, mono + 16, sizeof pass_camera[pc].view);
        }
    } else if (pc >= 0 && det3(mono + 16) > 0) game = pass_camera[pc].view, source = 2;
    else return false;
    static int trace = -1;  // SKYRECOMP_VR_CAM_TRACE=1: how often each source is used
    static long counts[3];
    if (trace < 0) trace = getenv("SKYRECOMP_VR_CAM_TRACE") != NULL;
    if (trace && ++counts[source] && (counts[0] + counts[1] + counts[2]) % 4000 == 0)
        fprintf(stderr, "[vr-rig] camera from game memory %ld, from the block %ld, from the last camera block %ld\n",
                counts[0], counts[1], counts[2]);
    memcpy(g_rig.game_view, game, sizeof g_rig.game_view);
    g_rig.have_game_view = true;
    VrRigAnchor a;
    vr_rig_current_anchor(&a, game);
    if (g_rig.mode == 1 && g_swim_raise > 0.0f) {
        // Room-scale tracking is applied after the anchor. A crouch can therefore cancel the fixed anchor raise and
        // put the real headset eye back through the surface. Move the anchor just enough that this eye (including
        // its tracked vertical offset) never gets closer than SWIM_EYE_RAISE to the game's surface point.
        float rel[9], off[3], world_off[3];
        vr_rig_relative(reference, eye, vr_rig_units_per_metre(), rel, off);
        m_vec(a.rotation, off, world_off);
        const float min_y = g_rig.link[1] + SWIM_EYE_RAISE;
        if (a.origin[1] + world_off[1] < min_y) a.origin[1] = min_y - world_off[1];
    }
    float view[12];
    if (!vr_rig_view(&a, reference, eye, vr_rig_units_per_metre(), view)) return false;
    const float near = g_rig.mode == 1 ? near_units() : 0.0f;
    if (!direct && source != 1 && planar_reflection(mono + 16, game)) {
        // The block draws the reflection in a horizontal mirror: Vm = F Vg R (R the world's reflection in the plane,
        // F = diag(1,-1,1) turning the image upside down, as the game samples it). The eye's version is F Ve R, so
        // C = F Ve R Vm^-1 = (F Ve) (F Vg)^-1. Plain C = Ve Vg^-1 cancelled the mirror: the pass drew an ordinary eye
        // view, so the floor showed what lies under it (B400's dark water wedge) and lakes an offset panel.
        float fview[12], fgame[12];
        memcpy(fview, view, sizeof fview);
        memcpy(fgame, game, sizeof fgame);
        for (int i = 4; i < 8; i++) fview[i] = -fview[i], fgame[i] = -fgame[i];
        // (the image is upside down: the headset's uneven up / down field of view swaps, or the reflection's
        // bottom would lack what the floor samples there)
        const float ffov[4] = {fov[0], fov[1], -fov[3], -fov[2]};
        return sky_vr_camera_with_view(mono, fgame, fview, ffov, near, out);
    }
    return direct ? sky_vr_camera_direct(mono, view, fov, near, out)
                  : sky_vr_camera_with_view(mono, game, view, fov, near, out);
#endif
}

bool vr_rig_eye_camera(const float mono[28], const float reference[7], const float eye[7], const float fov[4],
                       float out[28]) {
    return eye_camera(mono, reference, eye, fov, out, false);
}

bool vr_rig_eye_camera_direct(const float mono[28], const float reference[7], const float eye[7], const float fov[4],
                              float out[28]) {
    return eye_camera(mono, reference, eye, fov, out, true);
}

typedef struct { uint64_t hash, count, size; } LinkMesh;
static LinkMesh g_link_mesh[LINK_MESH_CACHE];

// A cheap stable identity for immutable vertex data. Addresses alone may be recycled after an area load. Sampling
// the beginning, middle and end distinguishes meshes without hashing megabytes of vertices every draw.
static uint64_t mesh_hash(uint64_t addr, uint64_t size) {
    if (!addr || !size) return 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
    uint64_t h = 1469598103934665603ull;
    const uint64_t sample = size < 64 ? size : 64;
    const uint64_t start[3] = {0, size > sample ? (size - sample) / 2 : 0, size > sample ? size - sample : 0};
    for (int s = 0; s < 3; s++)
        for (uint64_t i = 0; i < sample; i++) h = (h ^ p[start[s] + i]) * 1099511628211ull;
    return h;
}

static bool known_link_mesh(uint64_t hash, uint64_t count, uint64_t size) {
    for (int i = 0; i < LINK_MESH_CACHE; i++)
        if (g_link_mesh[i].hash == hash && g_link_mesh[i].count == count && g_link_mesh[i].size == size) return true;
    return false;
}

static void remember_link_mesh(uint64_t hash, uint64_t count, uint64_t size) {
    if (!hash || known_link_mesh(hash, count, size)) return;
    static int next;
    g_link_mesh[next] = (LinkMesh){hash, count, size};
    next = (next + 1) % LINK_MESH_CACHE;
}

bool vr_rig_is_link_draw(const float *slot0, uint64_t size0, const float *bones, uint64_t bone_bytes,
                         uint64_t count, uint64_t vb, uint64_t vb_size) {
#if defined(__APPLE__)
    (void)slot0; (void)size0; (void)bones; (void)bone_bytes; (void)count; (void)vb; (void)vb_size;
    return false;
#else
    static int show = -1;
    if (show < 0) show = getenv("SKYRECOMP_VR_SHOW_LINK") != NULL;
    if (show || g_rig.mode != 1 || !g_rig.have_game_view || !slot0) return false;
    if (!sky_vr_is_scene_projection(slot0, size0)) return false;  // (the shadow pass keeps Link: his shadow stays)
    // Link's eye in game-camera space; his bones sit around it (head to feet), any other character's farther away.
    const float *v = g_rig.game_view;
    float e[3];
    for (int r = 0; r < 3; r++) e[r] = v[r * 4] * g_rig.link[0] + v[r * 4 + 1] * g_rig.link[1] + v[r * 4 + 2] * g_rig.link[2] + v[r * 4 + 3];
    // Rigid draws include equipment and effects. Proximity cannot distinguish those from Link, so leave them alone;
    // the first-person near plane clips head intersections.
    if (!bones || bone_bytes < LINK_BODY_MIN_PALETTE * 48) return false;
    const int n = (int)(bone_bytes / 48);
    const uint64_t hash = mesh_hash(vb, vb_size);
    if (known_link_mesh(hash, count, vb_size)) {
        if (g_water) g_water_hits_now++;
        return true;
    }
    for (int i = 0; i < n; i++) {
        const float *b = bones + i * 12;
        const float dx = b[3] - e[0], dy = b[7] - e[1], dz = b[11] - e[2];
        if (dx * dx + dy * dy + dz * dz < LINK_BONE_RADIUS * LINK_BONE_RADIUS) {
            remember_link_mesh(hash, count, vb_size);
            if (g_water) g_water_hits_now++;
            return true;
        }
    }
    return false;
#endif
}

// ---- culling: the game culls with its own camera; in VR it has to cover the union of both eyes' frusta, widened by
// how far the eyes point away from the game camera (a turned head). Kept below 80 degrees
// per side (the culling math needs a finite tangent); turning further than that can still cull at the edges.
static _Atomic float g_cull_fovy, g_cull_aspect;
static _Atomic uint64_t g_cull_frame;
static pthread_mutex_t g_cull_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_cull_view[12], g_cull_tan[4];  // (under g_cull_lock)
static uint64_t g_cull_view_frame;

// Whether the cull view stands on the real game camera (from game memory, matched by the draws: gameplay). On the
// title / file select another camera draws and the rig's camera is a substitute (a pass's plain camera block): the
// headset frustum built on it culled objects the eyes were looking at (islands flickering, vanishing).
bool vr_rig_cull_trusted(void) { return memory_recent(); }

bool vr_rig_cull_view(float view[12], float tangents[4]) {
    extern uint64_t renderer_present_count(void);
    pthread_mutex_lock(&g_cull_lock);
    const bool ok = g_cull_view_frame && renderer_present_count() + 1 <= g_cull_view_frame + 30;
    if (ok) memcpy(view, g_cull_view, sizeof g_cull_view), memcpy(tangents, g_cull_tan, sizeof g_cull_tan);
    pthread_mutex_unlock(&g_cull_lock);
    return ok;
}

void vr_rig_update_cull(const float reference[7], const float eye0[7], const float fov0[4], const float eye1[7],
                        const float fov1[4]) {
    (void)eye1;
    if (!g_rig.have_game_view) return;
    const float tl = fminf(tanf(fov0[0]), tanf(fov1[0])), tr = fmaxf(tanf(fov0[1]), tanf(fov1[1]));
    const float tu = fmaxf(tanf(fov0[2]), tanf(fov1[2])), td = fminf(tanf(fov0[3]), tanf(fov1[3]));
    float half_h = atanf(fmaxf(-tl, tr)), half_v = atanf(fmaxf(tu, -td));
    VrRigAnchor a;
    vr_rig_current_anchor(&a, g_rig.game_view);
    float rel[9], off[3], rw[9], f[3];
    vr_rig_relative(reference, eye0, 1.0f, rel, off);
    m_mul(a.rotation, rel, rw);
    m_vec(rw, (const float[3]){0, 0, -1}, f);
    const float c[3] = {-g_rig.game_view[8], -g_rig.game_view[9], -g_rig.game_view[10]};
    const float yaw_off = fabsf(remainderf(atan2f(f[0], f[2]) - atan2f(c[0], c[2]), 2.0f * (float)M_PI));
    const float pitch_off = fabsf(asinf(fmaxf(-1, fminf(1, f[1]))) - asinf(fmaxf(-1, fminf(1, c[1]))));
    const float margin = 5.0f * (float)(M_PI / 180.0), limit = 80.0f * (float)(M_PI / 180.0);
    half_h = fminf(half_h + yaw_off + margin, limit);
    half_v = fminf(half_v + pitch_off + margin, limit);
    {   // eye 0's view in the game world and both eyes' field (vr_rig_cull_view, runtime/hle/vr_cull.c)
        float view[12];
        if (vr_rig_view(&a, reference, eye0, vr_rig_units_per_metre(), view)) {
            extern uint64_t renderer_present_count(void);
            pthread_mutex_lock(&g_cull_lock);
            memcpy(g_cull_view, view, sizeof view);
            g_cull_tan[0] = tl, g_cull_tan[1] = tr, g_cull_tan[2] = td, g_cull_tan[3] = tu;
            g_cull_view_frame = renderer_present_count() + 1;
            pthread_mutex_unlock(&g_cull_lock);
        }
    }
    if (!isfinite(half_h) || !isfinite(half_v)) return;
    extern uint64_t renderer_present_count(void);
    atomic_store(&g_cull_fovy, 2.0f * half_v);
    atomic_store(&g_cull_aspect, tanf(half_h) / tanf(half_v));
    atomic_store(&g_cull_frame, renderer_present_count() + 1);
}

bool vr_rig_cull_fov(float *fovy_radians, float *aspect) {
    extern uint64_t renderer_present_count(void);
    const uint64_t at = atomic_load(&g_cull_frame);
    if (!at || renderer_present_count() + 1 > at + 30) return false;  // (not in VR, or no stereo frame lately)
    if (fovy_radians) *fovy_radians = atomic_load(&g_cull_fovy);
    if (aspect) *aspect = atomic_load(&g_cull_aspect);
    return true;
}

