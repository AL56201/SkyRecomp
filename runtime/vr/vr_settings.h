// User-facing VR configuration. This module does not depend on the VR renderer or rig.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VRConfig {
    float resolution_scale
#ifdef __cplusplus
        = 1.0f
#endif
        ;                                // fraction of OpenXR's recommended per-eye resolution (0.5..2.0)
    bool desktop_mirror
#ifdef __cplusplus
        = true
#endif
        ;                                // draw the left eye in the desktop window while VR is running
    bool dynamic_resolution
#ifdef __cplusplus
        = false
#endif
        ;                                // adapt VR resolution to the OpenXR display-time budget
    float dynamic_resolution_min
#ifdef __cplusplus
        = 0.7f
#endif
        ;                                // lowest adaptive resolution multiplier (0.5..resolution_scale)
    float world_scale
#ifdef __cplusplus
        = 100.0f
#endif
        ;                                // game units per metre (50..200)
    int view_mode
#ifdef __cplusplus
        = 0
#endif
        ;                                // 0: third-person follow, 1: first-person head
    float camera_height_offset
#ifdef __cplusplus
        = 0.0f
#endif
        ;                                // game units (-50..50)
    float camera_distance_offset
#ifdef __cplusplus
        = 0.0f
#endif
        ;                                // game units (-100..100)
    bool level_horizon
#ifdef __cplusplus
        = true
#endif
        ;
    int move_direction
#ifdef __cplusplus
        = 0
#endif
        ;                                // left stick forward: 0 where the head looks, 1 / 2 the left / right controller
} VRConfig;

// Always contains a validated configuration. LoadVRConfig resets it to defaults first.
extern VRConfig g_vr_config;

void ResetVRConfig(void);
void LoadVRConfig(const char *filepath);
void SaveVRConfig(const char *filepath);

#ifdef __cplusplus
} // extern "C"
#endif
