// The VR controllers as the game's Joy-Con pair: buttons and sticks, and the motion sensors (vr_input.c feeds the
// translator; runtime/hle/input.c and runtime/hle/motion.c read these on the game thread).
#include "vr_internal.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#if SKYRECOMP_HAS_OPENXR
#include "hle.h"
#include "nvn/vulkan/JoyConTranslator.h"
#include "vr_pointer.h"

static bool controllers_off(void) {  // (test) SKYRECOMP_VR_NO_INPUT=1: controllers ignored, for scripted runs
    static int off = -1;
    if (off < 0) off = getenv("SKYRECOMP_VR_NO_INPUT") != NULL;
    return off;
}

_Static_assert(sizeof(VrJoyConState) == sizeof(SkyJoyConState), "the same layout as JoyConTranslator.h");
_Static_assert(sizeof(VrJoyConSensor) == sizeof(SkyJoyConSensor), "the same layout as JoyConTranslator.h");

// ---- the menu pointer: on the title / file select screens (no Link in the world) and while a menu pauses the game
// (the gear menu, the map...), the game's cursor goes where the right controller's ray meets the floating HUD panel.
// The game turns the right Joy-Con's orientation, relative to its canonical centre, into a cursor like a flat screen:
// offset = K tan(angle), with K = 1.298 screen widths horizontally (yaw about the Joy-Con's z) and 2.316 screen
// heights vertically (pitch about x; down positive) - measured 2026-10-26 with SKYRECOMP_JOYCON_TEST=point. The
// panel's hit (u, v) gives those angles back. Elsewhere the real orientation goes to the game (the sword follows it).
// SKYRECOMP_VR_PANEL_POINTER=0 turns it off.
#define POINTER_KX 1.298f
#define POINTER_KY 2.316f
static pthread_mutex_t g_ptr_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_ptr_aim[16], g_ptr_hud[16];  // (row-major world-from-local: the right aim pose; the panel, half extents)
static bool g_ptr_aim_ok, g_ptr_hud_ok;

// The Joy-Con direction (rows: its axes in the player's frame) that puts the game's cursor at (u, v) of the screen
// (0..1, top left): a turn about its own z then x from the canonical centre.
static void pointer_direction(float u, float v, float out[9]) {
    const float a = atanf((u - 0.5f) / POINTER_KX), b = atanf((v - 0.5f) / POINTER_KY);
    const float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
    const float yaw[9] = {ca, -sa, 0, sa, ca, 0, 0, 0, 1}, pitch[9] = {1, 0, 0, 0, cb, -sb, 0, sb, cb};
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++)
            out[r * 3 + k] = pitch[r * 3] * yaw[k] + pitch[r * 3 + 1] * yaw[3 + k] + pitch[r * 3 + 2] * yaw[6 + k];
}

// (vr_input.c, once per frame) The right controller's aim pose in the local space.
void vr_pointer_set_aim(bool tracked, const float q[4], const float p[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float m[16] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), p[0],
                         2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), p[1],
                         2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), p[2],
                         0, 0, 0, 1};
    pthread_mutex_lock(&g_ptr_lock);
    g_ptr_aim_ok = tracked;
    if (tracked) memcpy(g_ptr_aim, m, sizeof m);
    pthread_mutex_unlock(&g_ptr_lock);
}

// (vr.c, each frame the HUD panel is placed) The panel the menu pointer aims at (world-from-panel, half extents).
void vr_controls_set_hud_panel(const float m[16]) {
    pthread_mutex_lock(&g_ptr_lock);
    memcpy(g_ptr_hud, m, sizeof g_ptr_hud);
    g_ptr_hud_ok = true;
    pthread_mutex_unlock(&g_ptr_lock);
}

static bool panel_pointer_on(void) {
    static int env = -1;
    if (env < 0) env = getenv("SKYRECOMP_VR_PANEL_POINTER") ? atoi(getenv("SKYRECOMP_VR_PANEL_POINTER")) != 0 : 1;
    extern bool vk_hud_layer_enabled(void);
    extern bool player_eye(float eye[3]);
    if (!env || !vk_hud_layer_enabled()) return false;
    return *(volatile uint32_t *)(uintptr_t)(MAIN_BASE + 0x1827370) != 0  // (a menu pauses the game)
           || !player_eye(NULL);  // (no Link: the title, file select, loading)
}

static bool panel_pointer(SkyJoyConSensor *s) {
    pthread_mutex_lock(&g_ptr_lock);
    const bool on = panel_pointer_on() && g_ptr_aim_ok && g_ptr_hud_ok;
    float aim[16], hud[16];
    memcpy(aim, g_ptr_aim, sizeof aim);
    memcpy(hud, g_ptr_hud, sizeof hud);
    pthread_mutex_unlock(&g_ptr_lock);
    if (!on) return false;
    VRPointerState p;
    if (!ComputeVRPointerIntersection(&p, aim, hud) || !p.intersects) return false;  // (off the panel: the real pose)
    pointer_direction(p.ui_space_x, p.ui_space_y, s->direction);
    static int logged;
    if (!logged++) LOG("[vr] menu pointer: the game's cursor follows the right controller's ray on the HUD panel\n");
    return true;
}

bool vr_joycon_state(VrJoyConState *state) {
    SkyJoyConState s;
    extern bool motion_joycon_mode(void);
    if (!state || controllers_off() || !motion_joycon_mode() || !sky_joycon_state(&s)) return false;
    memcpy(state, &s, sizeof s);
    return true;
}

// Skyward Strike asks for a nearly upright, steady Joy-Con. Real VR grips have controller-specific pitch offsets
// and considerably more resting jitter than a calibrated Joy-Con. Once the sword is already raised mostly upward,
// report the intended cardinal pose and remove only low-rate jitter; swings below this cone remain completely raw.
static void skyward_strike_assist(SkyJoyConSensor *s) {
    if (s->direction[5] < 0.72f) return;  // Joy-Con +Y follows the controller's pointing axis.
    float x = s->direction[0], y = s->direction[1];
    const float n = sqrtf(x * x + y * y);
    if (n > 1e-4f) x /= n, y /= n; else x = 1.0f, y = 0.0f;
    // Rows are the Joy-Con x/y/z axes in player space; x cross blade gives the remaining orthogonal axis.
    s->direction[0] = x;    s->direction[1] = y;    s->direction[2] = 0.0f;
    s->direction[3] = 0.0f; s->direction[4] = 0.0f; s->direction[5] = 1.0f;
    s->direction[6] = y;    s->direction[7] = -x;   s->direction[8] = 0.0f;
    s->accel[0] = 0.0f; s->accel[1] = -1.0f; s->accel[2] = 0.0f;
    const float jitter = 0.08f;  // revolutions/s; preserve deliberate rotations and every real swing.
    for (int i = 0; i < 3; i++) if (fabsf(s->gyro[i]) < jitter) s->gyro[i] = 0.0f;
}

bool vr_joycon_sensor(int device, VrJoyConSensor *sensor) {
    SkyJoyConSensor s;
    extern bool motion_joycon_mode(void);
    if (!sensor || controllers_off() || !motion_joycon_mode() || !sky_joycon_sensor(device, &s)) return false;
    if (device == 1 && !panel_pointer(&s)) skyward_strike_assist(&s);  // (the right Joy-Con: the pointer, the sword)
    memcpy(sensor, &s, sizeof s);
    return true;
}

#else
void vr_controls_set_hud_panel(const float m[16]) { (void)m; }
bool vr_joycon_state(VrJoyConState *state) { (void)state; return false; }
bool vr_joycon_sensor(int device, VrJoyConSensor *sensor) { (void)device; (void)sensor; return false; }
#endif
