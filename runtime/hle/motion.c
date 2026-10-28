// Joy-Con motion sensors (nn::hid SixAxisSensor) and the controller style the game sees.
//
// Desktop: the game sees one Pro Controller whose sensor rests level (its button controls). VR (or
// SKYRECOMP_JOYCON=1; =0 forces the Pro Controller): a pair of detached Joy-Cons (NpadJoyDual), the style Skyward
// Sword HD plays with motion controls; each Joy-Con's sensor is the matching VR controller (runtime/vr.h
// vr_joycon_sensor, runtime/nvn/vulkan/JoyConTranslator.h for the conventions). The game reads both sensors every
// frame, recognises swings / thrusts / shield bashes itself, and points with Joy-Con (R).
//
// The style follows the live VR setting; input.c signals the SDK's style-change event when VR is toggled.
#include <math.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../frame_rate.h"
#include "../vr/vr.h"
#include "../vr/vr_settings.h"

enum { STYLE_FULLKEY = 1u << 0, STYLE_JOY_DUAL = 1u << 2 };  // NpadStyleTag bits
enum { STYLE_INDEX_FULLKEY = 3, STYLE_INDEX_JOY_DUAL = 5 };   // NpadStyleIndex
enum { DEVICE_LEFT = 0, DEVICE_RIGHT = 1, DEVICE_NONE = 2 };  // SixAxisSensorHandle device index

// nn::hid::SixAxisSensorHandle (4 bytes)
typedef struct { uint8_t style_index, player, device, pad; } SensorHandle;

// nn::hid::SixAxisSensorState (0x60 bytes). Units: acceleration in g, angular velocity in revolutions per second,
// angle in revolutions, direction = the sensor's axes as rows.
typedef struct {
    int64_t delta_ns, sampling;
    float accel[3], gyro[3], angle[3], direction[9];
    uint32_t attributes, reserved;
} SensorState;
_Static_assert(sizeof(SensorState) == 0x60, "nn::hid::SixAxisSensorState");
_Static_assert(offsetof(SensorState, accel) == 0x10 && offsetof(SensorState, direction) == 0x34, "SixAxisSensorState");

static _Atomic int auto_joycon;

// This coordinator owns the entire legacy sequence: create the pointing/Y modal, wait, request recalibration, wait
// again, then tear it down. Bypass the coordinator itself in VR, before any modal/audio/calibration state exists.
// JoyDual HID remains active throughout; desktop/controller behavior is untouched.
#define LEGACY_CALIBRATION_COORDINATOR 0x7000dd2a68ull
#define CONTROLLER_MANAGER_PTR (MAIN_BASE + 0x1827380)
static GuestFn legacy_calibration_coordinator;
static void vr_skip_legacy_calibration(Cpu *c) {
    if (!vr_requested()) {
        legacy_calibration_coordinator(c);
        return;
    }
    static int logged;
    if (!logged++) LOG("[motion] VR: legacy manual-calibration coordinator disabled\n");

    // Preserve the coordinator's unconditional tail update of the controller manager.
    c->x[0] = *(uint64_t *)(uintptr_t)CONTROLLER_MANAGER_PTR;
    if (!c->x[0]) return;
    uint64_t update = *(uint64_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)c->x[0] + 0x30);
    if (update) guest_call(c, update);
}

// TEMP (RE) SKYRECOMP_TEMP_MOTION_GATE=1: f_7000a17aa8 / f_7000a17be0 (player: "motion input active?") logged on change
// with the player's action (+0x468) and its controller block (+0x638c: +0x64..+0x6f, +0x78)
static GuestFn temp_gate_aa8, temp_gate_be0;
static void temp_gate_log(const char *which, Cpu *c, uint64_t player) {
    static char last[2][160];
    char line[160];
    const uint8_t *k = (const uint8_t *)(uintptr_t)(player + 0x638c);
    snprintf(line, sizeof line, "ret %llu action %#x b64 %02x%02x%02x%02x b68 %02x%02x%02x%02x b6c %02x%02x u78 %u",
             (unsigned long long)(c->x[0] & 1), *(const uint32_t *)(uintptr_t)(player + 0x468), k[0x64], k[0x65], k[0x66],
             k[0x67], k[0x68], k[0x69], k[0x6a], k[0x6b], k[0x6c], k[0x6d], *(const uint16_t *)(k + 0x78));
    const int i = which[0] == 'a';
    if (strcmp(line, last[i])) { LOG("[gate] %s %s\n", which, line); strcpy(last[i], line); }
}
static void temp_gate_aa8_hook(Cpu *c) { const uint64_t p = c->x[0]; temp_gate_aa8(c); temp_gate_log("aa8", c, p); }
static void temp_gate_be0_hook(Cpu *c) { const uint64_t p = c->x[0]; temp_gate_be0(c); temp_gate_log("be0", c, p); }

void motion_vr_init(void) {
    if (getenv("SKYRECOMP_TEMP_MOTION_GATE")) {
        temp_gate_aa8 = dispatch_lookup(0x7000a17aa8ull);
        temp_gate_be0 = dispatch_lookup(0x7000a17be0ull);
        if (temp_gate_aa8) hle_override(0x7000a17aa8ull, temp_gate_aa8_hook, "temp_gate_aa8");
        if (temp_gate_be0) hle_override(0x7000a17be0ull, temp_gate_be0_hook, "temp_gate_be0");
    }
    legacy_calibration_coordinator = dispatch_lookup(LEGACY_CALIBRATION_COORDINATOR);
    if (legacy_calibration_coordinator)
        hle_override(LEGACY_CALIBRATION_COORDINATOR, vr_skip_legacy_calibration, "vr_skip_legacy_motion_calibration");
    else
        LOG("[motion] warning: legacy calibration hook unavailable\n");
}

// VR always plays with motion controls (user: button controls must not be an option in VR - they lose the
// motion): the Joy-Con pair whenever VR is on, whatever was used last. g_vr_config.control_source (the old "VR
// Control Source" choice) is no longer read.
static int forced_mode(void) { return vr_requested() ? 1 : -1; }

bool motion_joycon_mode(void) {
    static int override = -2, last = -1;
    if (override == -2) {
        const char *e = getenv("SKYRECOMP_JOYCON");
        override = e ? atoi(e) != 0 : -1;
    }
    const int forced = forced_mode();
    const int on = forced >= 0 ? forced : override >= 0 ? override : atomic_load(&auto_joycon);  // (VR first)
    if (on != last) {
        LOG("[motion] controller style: %s\n", on ? "Joy-Con pair (motion controls)" : "Pro Controller");
        last = on;
    }
    return on;
}

// Auto mode follows the last input source that the player actually used. SDL pads call this with their database
// classification; OpenXR calls it when tracked motion-controller input becomes active.
void motion_select_input(bool joycon) {
    if (forced_mode() >= 0 || getenv("SKYRECOMP_JOYCON")) return;
    if (atomic_exchange(&auto_joycon, joycon) == joycon) return;
    (void)motion_joycon_mode();
    extern void input_controller_style_changed(void);
    input_controller_style_changed();
}

void motion_input_config_changed(void) {
    (void)motion_joycon_mode();
    extern void input_controller_style_changed(void);
    input_controller_style_changed();
}

// Each distinct call shape logged once (SKYRECOMP_JOYCON_TRACE=1).
static void note(const char *what, uint32_t a, uint32_t b, uint32_t c) {
    static struct { const char *what; uint32_t a, b, c; } seen[64];
    static int n, on = -1;
    if (on < 0) on = getenv("SKYRECOMP_JOYCON_TRACE") != NULL;
    if (!on) return;
    for (int i = 0; i < n; i++)
        if (seen[i].what == what && seen[i].a == a && seen[i].b == b && seen[i].c == c) return;
    if (n < 64) seen[n++] = (typeof(seen[0])){what, a, b, c};
    LOG("[motion] %s %#x %#x %#x\n", what, a, b, c);
}

// (test) SKYRECOMP_JOYCON_TEST=sweep: Joy-Con (R) turns +-20 degrees about its x, y, then z axis, 120 presented
// frames each (the pointer moves: z horizontally, x vertically); =gyro: gyro only, +0.1 rev/s about z for frames
// 2700..2759 (the pointer stays: the game points with `direction`).
static bool test_sample(uint8_t device, SensorState *s) {
    static const char *test_r = (const char *)1, *test_l = (const char *)1;  // (=_LEFT: the same for Joy-Con (L))
    if (test_r == (const char *)1) test_r = getenv("SKYRECOMP_JOYCON_TEST"), test_l = getenv("SKYRECOMP_JOYCON_TEST_LEFT");
    const char *test = device == DEVICE_RIGHT ? test_r : device == DEVICE_LEFT ? test_l : NULL;
    if (!test) return false;
    extern uint64_t renderer_present_count(void);
    const uint64_t f = renderer_present_count();
    if (!strcmp(test, "gyro")) {
        if (f >= 2700 && f < 2760) s->gyro[2] = 0.1f;
        return true;
    }
    // (test) =point:<yaw>,<pitch>[,<frame>]: held turned that many degrees about z, then about x (from that frame:
    // straight before it, so the game's Y calibration can be made first)
    // =seq:<yaw>,<pitch>,<frame>[,<gx>,<gy>,<gz>[,<ax>,<ay>,<az>]]/...: each from its frame (straight before the
    // first), with an optional gyroscope rate (revolutions per second about the Joy-Con's x, y, z) and acceleration
    // (g; a swing) during it
    float yaw_deg = 0, pitch_deg = 0, from = 0, gyro[3] = {0, 0, 0}, accel[3] = {0, 0, -1};
    bool has_accel = false;
    if (!strncmp(test, "seq:", 4)) {
        bool any = false;
        for (const char *p = test + 4; p && *p; p = strchr(p, '/') ? strchr(p, '/') + 1 : NULL) {
            float y, pi, fr, g[3] = {0, 0, 0}, ac[3] = {0, 0, -1};
            const int n = sscanf(p, "%f,%f,%f,%f,%f,%f,%f,%f,%f", &y, &pi, &fr, &g[0], &g[1], &g[2], &ac[0], &ac[1], &ac[2]);
            if (n >= 3 && (float)f >= fr) {
                yaw_deg = y, pitch_deg = pi, any = true, memcpy(gyro, g, sizeof g), memcpy(accel, ac, sizeof ac);
                has_accel = n >= 9;
            }
        }
        if (!any) return true;
        memcpy(s->gyro, gyro, sizeof gyro);
        if (has_accel) memcpy(s->accel, accel, sizeof accel);
        from = 0;
    }
    if (!strncmp(test, "seq:", 4) || sscanf(test, "point:%f,%f,%f", &yaw_deg, &pitch_deg, &from) >= 2) {
        if ((float)f < from) return true;
        const float a = yaw_deg * (float)(M_PI / 180.0), b = pitch_deg * (float)(M_PI / 180.0);
        const float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
        const float yaw[9] = {ca, -sa, 0, sa, ca, 0, 0, 0, 1}, pitch[9] = {1, 0, 0, 0, cb, -sb, 0, sb, cb};
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++)
                s->direction[r * 3 + k] = pitch[r * 3 + 0] * yaw[0 * 3 + k] + pitch[r * 3 + 1] * yaw[1 * 3 + k] +
                                          pitch[r * 3 + 2] * yaw[2 * 3 + k];
        s->angle[0] = b / (float)(2 * M_PI);  // (the integrated rotation, revolutions, as the sweep test)
        s->angle[2] = a / (float)(2 * M_PI);
        return true;
    }
    const int axis = (int)(f / 120 % 3), i = (axis + 1) % 3, j = (axis + 2) % 3;
    const double w = 2 * M_PI / 120.0, amp = 20.0 * M_PI / 180.0, ph = w * (double)(f % 120);
    const float a = (float)(amp * sin(ph)), c = cosf(a), sn = sinf(a);
    s->direction[i * 3 + i] = c;
    s->direction[i * 3 + j] = -sn;
    s->direction[j * 3 + i] = sn;
    s->direction[j * 3 + j] = c;
    s->gyro[axis] = (float)(amp * w * 60.0 * cos(ph) / (2 * M_PI));
    s->angle[axis] = a / (float)(2 * M_PI);
    return true;
}

// One Joy-Con sensor sample: the VR controller, else lying level at rest.
static void motion_sample(uint8_t device, SensorState *s) {
    s->accel[2] = -1.0f;
    s->direction[0] = s->direction[4] = s->direction[8] = 1.0f;
    if (test_sample(device, s)) return;
    VrJoyConSensor v;
    if (device > DEVICE_RIGHT || !vr_joycon_sensor(device, &v)) return;
    memcpy(s->accel, v.accel, sizeof s->accel);
    memcpy(s->gyro, v.gyro, sizeof s->gyro);
    memcpy(s->angle, v.angle, sizeof s->angle);
    memcpy(s->direction, v.direction, sizeof s->direction);
    if (v.delta_ns > 0) s->delta_ns = v.delta_ns;
}

uint32_t motion_style_set(void) { return motion_joycon_mode() ? STYLE_JOY_DUAL : STYLE_FULLKEY; }

// GetNpadStyleSet(NpadId): the styles attached to a player.
HLE_FUNC("_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set) {
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[0];
    note("GetNpadStyleSet", id, 0, 0);
    c->x[0] = id == 0 ? motion_style_set() : 0;
}

// GetSixAxisSensorHandles(out, count, NpadId, style): one sensor for a Pro Controller, two for a Joy-Con pair.
HLE_FUNC("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_sixaxis_handles) {
    SensorHandle *out = (SensorHandle *)(uintptr_t)c->x[0];
    int count = (int)c->x[1];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    uint32_t style = (uint32_t)c->x[3];
    note("GetSixAxisSensorHandles", (uint32_t)count, id, style);
    int n = 0;
    if (id == 0 && out) {
        if ((style & STYLE_JOY_DUAL) && motion_joycon_mode()) {
            if (n < count) out[n++] = (SensorHandle){STYLE_INDEX_JOY_DUAL, 0, DEVICE_LEFT, 0};
            if (n < count) out[n++] = (SensorHandle){STYLE_INDEX_JOY_DUAL, 0, DEVICE_RIGHT, 0};
        } else if (count >= 1) {
            out[n++] = (SensorHandle){STYLE_INDEX_FULLKEY, 0, DEVICE_NONE, 0};
        }
    }
    c->x[0] = (uint64_t)n;
}

HLE_FUNC("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_start_sixaxis) {
    const SensorHandle *h = (const SensorHandle *)(uintptr_t)c->x[0];
    note("StartSixAxisSensor", h->style_index, h->player, h->device);
    c->x[0] = 0;
}
HLE_FUNC("_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_stop_sixaxis) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn3hid25EnableSixAxisSensorFusionERKNS0_19SixAxisSensorHandleEb", hid_enable_sixaxis_fusion) {
    note("EnableSixAxisSensorFusion", (uint32_t)c->x[1], 0, 0);
    c->x[0] = 0;
}
HLE_FUNC("_ZN2nn3hid32SetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleEff", hid_set_sixaxis_fusion) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE", hid_set_gyro_drift) { c->x[0] = 0; }

HLE_FUNC("_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", hid_get_sixaxis_state) {
    SensorState *s = (SensorState *)(uintptr_t)c->x[0];
    const SensorHandle *h = (const SensorHandle *)(uintptr_t)c->x[1];
    static int64_t sampling;
    note("GetSixAxisSensorState", h->style_index, h->player, h->device);
    memset(s, 0, sizeof *s);
    s->delta_ns = (int64_t)rt_frame_period_ns();
    s->sampling = ++sampling;
    s->attributes = 1;  // IsConnected
    if (motion_joycon_mode()) {
        motion_sample(h->device, s);
    } else {
        s->accel[1] = -1.0f;
        s->direction[0] = s->direction[4] = s->direction[8] = 1.0f;
    }
}

// SixAxisSensorState::GetQuaternion: the orientation of `direction` as a quaternion (x, y, z, w).
HLE_FUNC("_ZNK2nn3hid18SixAxisSensorState13GetQuaternionEPNS_4util4neon12Vector4fTypeE", hid_sixaxis_quat) {
    const SensorState *s = (const SensorState *)(uintptr_t)c->x[0];
    float *q = (float *)(uintptr_t)c->x[1];
    if (q) {
        const float *m = s->direction;
        const float w = sqrtf(fmaxf(0.0f, 1.0f + m[0] + m[4] + m[8])) * 0.5f;
        const float k = w > 1e-4f ? 0.25f / w : 0.0f;
        q[0] = (m[5] - m[7]) * k;
        q[1] = (m[6] - m[2]) * k;
        q[2] = (m[1] - m[3]) * k;
        q[3] = w > 1e-4f ? w : 1.0f;
    }
    c->x[0] = 0;
}
