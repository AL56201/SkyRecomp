// Shared between the OpenXR core (vr.c) and the camera rig (vr_rig.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// (SKYRECOMP_NO_OPENXR: build without it even where the SDK is installed)
#if !defined(__APPLE__) && !defined(SKYRECOMP_NO_OPENXR) && __has_include(<openxr/openxr.h>) && \
    __has_include(<openxr/openxr_platform.h>)
#define SKYRECOMP_HAS_OPENXR 1
#else
#define SKYRECOMP_HAS_OPENXR 0
#endif

#include "vr.h"
#include "vr_rig.h"

// ---- vr.c (the OpenXR core)
const float *vr_reference_pose(void);         // the head pose the game camera stands for {qx, qy, qz, qw, px, py, pz}
bool vr_eye_pose(int eye, float pose[7]);     // this frame's eye pose (OpenXR local space)
bool vr_eye_tangents(int eye, float tan[4]);  // this frame's eye field: tangents left, right, up, down
