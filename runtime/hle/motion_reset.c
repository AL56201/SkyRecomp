// The direct-call gyro reset in main+0xdd1a68 also plays sound 0x13bd. The prompt coordinator is separate;
// suppressing SYS_GYRO_00 alone cannot silence this sound. gen.py chooses the existing quiet-reset branch in VR.
#include <SDL3/SDL_timer.h>
#include "../hle.h"
#include "../vr/vr.h"

static uint64_t last_quiet_frame;
static bool have_quiet_frame;
static _Thread_local bool temporary_gate;
static _Thread_local uint64_t reset_caller, reset_options[3];
static _Thread_local int reset_sound;

static bool quiet_enabled(void) {
    return vr_requested() && !ENV("SKYRECOMP_CALIBRATION_LEGACY_RESET");
}

int rt_sky_motion_quiet_reset(void) {
    const bool quiet = quiet_enabled();
    reset_sound = quiet ? 0 : 1;
    if (quiet) {
        extern uint64_t renderer_present_count(void);
        last_quiet_frame = renderer_present_count();
        have_quiet_frame = true;
    }
    return quiet;
}

uint64_t rt_sky_motion_reset_begin(Cpu *c) {
    // The audible branch leaves a three-frame duplicate-reset gate behind. The quiet branch clears it.
    // Reapply it only inside a duplicate call, then restore it, keeping feedback animation state untouched.
    temporary_gate = false;
    if (quiet_enabled() && (c->x[0] & 1) && have_quiet_frame) {
        extern uint64_t renderer_present_count(void);
        const uint64_t frame = renderer_present_count();
        uint8_t *gate = (uint8_t *)(uintptr_t)(MAIN_BASE + 0x192fdda);
        if (frame >= last_quiet_frame && frame - last_quiet_frame < 3 && !*gate) {
            *gate = 3;
            temporary_gate = true;
        }
    }
    reset_sound = -1;
    if (!ENV("SKYRECOMP_MOTION_RESET_TRACE")) return 0;
    reset_caller = c->x[30];
    for (int i = 0; i < 3; i++) reset_options[i] = c->x[i];
    return SDL_GetTicksNS();
}

void rt_sky_motion_reset_end(uint64_t start) {
    const uint64_t end = start ? SDL_GetTicksNS() : 0;
    if (temporary_gate) {
        *(uint8_t *)(uintptr_t)(MAIN_BASE + 0x192fdda) = 0;
        temporary_gate = false;
    }
    if (start) {
        // One buffered file write instead of stderr's per-character Windows writes in the measured routine.
        static FILE *trace;
        if (!trace) trace = fopen(ENV("SKYRECOMP_MOTION_RESET_TRACE"), "w");
        if (trace) {
            fprintf(trace, "%llu,%llu,%llx,%llu,%llu,%llu,%d\n", (unsigned long long)start,
                    (unsigned long long)end, (unsigned long long)reset_caller,
                    (unsigned long long)reset_options[0], (unsigned long long)reset_options[1],
                    (unsigned long long)reset_options[2], reset_sound);
            fflush(trace);
        }
    }
}

RT_ON_RESET(quiet_motion_reset_state) {
    have_quiet_frame = false;
    temporary_gate = false;
}
