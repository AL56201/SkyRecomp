// The VR controllers as the game's Joy-Con pair: buttons and sticks, and the motion sensors (vr_input.c feeds the
// translator; runtime/hle/input.c and runtime/hle/motion.c read these on the game thread).
#include "vr_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if SKYRECOMP_HAS_OPENXR
#include "nvn/vulkan/JoyConTranslator.h"

static bool controllers_off(void) {  // (test) SKYRECOMP_VR_NO_INPUT=1: controllers ignored, for scripted runs
    static int off = -1;
    if (off < 0) off = getenv("SKYRECOMP_VR_NO_INPUT") != NULL;
    return off;
}

_Static_assert(sizeof(VrJoyConState) == sizeof(SkyJoyConState), "the same layout as JoyConTranslator.h");
_Static_assert(sizeof(VrJoyConSensor) == sizeof(SkyJoyConSensor), "the same layout as JoyConTranslator.h");

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
    if (device == 1) skyward_strike_assist(&s);  // (the right Joy-Con: the sword)
    memcpy(sensor, &s, sizeof s);
    return true;
}

#else
bool vr_joycon_state(VrJoyConState *state) { (void)state; return false; }
bool vr_joycon_sensor(int device, VrJoyConSensor *sensor) { (void)device; (void)sensor; return false; }
#endif
