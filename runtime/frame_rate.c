#include "frame_rate.h"
#include "rt.h"
#include "settings.h"
#include "hle.h"

// VR (vr.c): the headset's nominal refresh rate while a session runs, else 0. With a high frame rate chosen, the game
// targets it (a 144 Hz headset: 144 FPS), and OpenXR's frame wait paces the frames.
int g_rt_headset_hz;

int rt_frame_rate(void) {
    // An active OpenXR session is the display clock. Requiring the separate
    // desktop frame-rate option to be changed left a 144 Hz headset fed by the
    // game's 60 Hz limiter, forcing SteamVR to synthesize most frames. The
    // high-rate hooks below retain real-time simulation speed.
    if (g_is_skyward && g_rt_headset_hz >= 60 && g_rt_headset_hz <= 360) return g_rt_headset_hz;
    if (!g_is_skyward || (g_settings.frame_rate != 120 && g_settings.frame_rate != 144)) return 60;
    return g_settings.frame_rate;
}
uint64_t rt_frame_period_ns(void) { return (1000000000ull + rt_frame_rate() / 2) / rt_frame_rate(); }
float rt_frame_scale(void) { return 60.0f / rt_frame_rate(); }
bool rt_headset_paced(void) { return g_rt_headset_hz > 0 && rt_frame_rate() > 60; }

static uint64_t last_step_ns;
static float sky_step;

float rt_sky_frame_step(float original) {
    const int rate = rt_frame_rate();
    if (rate <= 60) { last_step_ns = 0; return original; }
    uint64_t now = plat_now_ns(), elapsed = now - last_step_ns;
    sky_step = original * 60.0f / (float)rate;
    // Keep real-time speed below the target too; loading stalls must not become giant physics steps.
    if (last_step_ns && elapsed < 100000000ull)
        sky_step = elapsed >= 33333333ull ? 1.0f : (float)((double)elapsed * 30.0 / 1e9);
    last_step_ns = now;
    static unsigned calls;
    if (ENV("LARECOMP_LOG_FRAMEPACE") && ++calls % 120 == 0)
        LOG("[frame-rate] target %d step %.6f ms %.3f\n", rate, sky_step, sky_step * (1000.0f / 30));
    return sky_step;
}

float rt_sky_frame_ms(float original) {
    return rt_frame_rate() > 60 ? sky_step * (1000.0f / 30) : original;
}
