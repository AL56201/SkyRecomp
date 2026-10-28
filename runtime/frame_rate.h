#pragma once
#include <stdbool.h>
#include <stdint.h>

int rt_frame_rate(void);
uint64_t rt_frame_period_ns(void);
float rt_frame_scale(void);
bool rt_headset_paced(void);  // VR at a high frame rate: OpenXR's frame wait paces frames, not the game's limiter
extern int g_rt_headset_hz;   // (vr.c) the headset's nominal refresh rate while a session runs, else 0
float rt_sky_frame_step(float original);
float rt_sky_frame_ms(float original);
