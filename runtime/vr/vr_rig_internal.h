// The VR rig's internals (renderer thread): vr_rig.c (the camera rig, Link's own draws, culling).
// Not for other code: the public interface is vr_rig.h.
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "vr_rig.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846  // (strict ISO C leaves it out)
#endif

// ---- 3x3 rotations and 3x4 affine transforms (row-major)
static inline void m_vec(const float m[9], const float v[3], float out[3]) {
    float r[3];
    for (int i = 0; i < 3; i++) r[i] = m[i * 3] * v[0] + m[i * 3 + 1] * v[1] + m[i * 3 + 2] * v[2];
    memcpy(out, r, sizeof r);
}
static inline void m_mul(const float a[9], const float b[9], float out[9]) {
    float r[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    memcpy(out, r, sizeof r);
}
static inline void a_mul(const float a[12], const float b[12], float out[12]) {  // 3x4 affine product
    float r[12];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + (j == 3 ? a[i * 4 + 3] : 0);
    memcpy(out, r, sizeof r);
}
static inline void a_rigid_inverse(const float m[12], float out[12]) {  // inverse of m's rotation and translation
    float rot[9];
    for (int c = 0; c < 3; c++) {
        const float n = sqrtf(m[c] * m[c] + m[4 + c] * m[4 + c] + m[8 + c] * m[8 + c]) + 1e-12f;
        for (int r = 0; r < 3; r++) rot[r * 3 + c] = m[r * 4 + c] / n;
    }
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) out[r * 4 + c] = rot[c * 3 + r];
        out[r * 4 + 3] = -(rot[r] * m[3] + rot[3 + r] * m[7] + rot[6 + r] * m[11]);
    }
}
static inline bool a_inverse(const float m[12], float out[12]) {  // full inverse of a 3x4 affine (scale kept)
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (!(fabsf(det) > 1e-12f)) return false;
    const float r[9] = {A / det, -(b * i - c * h) / det, (b * f - c * e) / det,
                        B / det, (a * i - c * g) / det, -(a * f - c * d) / det,
                        C / det, -(a * h - b * g) / det, (a * e - b * d) / det};
    for (int k = 0; k < 3; k++) {
        out[k * 4] = r[k * 3], out[k * 4 + 1] = r[k * 3 + 1], out[k * 4 + 2] = r[k * 3 + 2];
        out[k * 4 + 3] = -(r[k * 3] * m[3] + r[k * 3 + 1] * m[7] + r[k * 3 + 2] * m[11]);
    }
    return true;
}
static inline void cam_point(const float view[12], const float w[3], float out[3]) {  // a world point in camera space
    for (int r = 0; r < 3; r++) out[r] = view[r * 4] * w[0] + view[r * 4 + 1] * w[1] + view[r * 4 + 2] * w[2] + view[r * 4 + 3];
}

// ---- vr_rig.c: this frame's rig state
typedef struct {
    int mode;                      // 0 3rd person, 1 1st person (only while Link's pose is known)
    bool have_game_view;
    float game_view[12];           // the last scene camera's view (row-major 3x4, world -> camera)
    float link[3];                 // Link's eye point (world)
} VrRigShared;
extern VrRigShared g_rig;
// A pose relative to the (yaw-only) reference: its rotation and its offset scaled by `units`.
void vr_rig_relative(const float reference[7], const float eye[7], float units, float rot[9], float offset[3]);
void vr_rig_current_anchor(VrRigAnchor *a, const float *game_view);  // the anchor the eyes are placed from
