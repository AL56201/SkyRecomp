#pragma once
#include <stdbool.h>
#include <math.h>
#include <string.h>

#define VR_FADE_OVERLAY_DISTANCE 0.25f
#define VR_FADE_OVERLAY_SIZE 4.0f

// SSHD's solid layout shader: projection rows 0..15, model rows 16..27, uniform RGBA 28..31.
// Validate all four transformed corners, rather than assuming the game used a unit rectangle.
static inline bool vr_fade_quad_color(const float *c, const float vertices[4][3], float rgba[4]) {
    unsigned corners = 0;
    for (int i = 0; i < 4; i++) {
        float v[4] = {0, 0, 0, 1}, clip[4] = {0};
        for (int row = 0; row < 3; row++) {
            v[row] = c[16 + row * 4 + 3];
            for (int k = 0; k < 3; k++) v[row] += c[16 + row * 4 + k] * vertices[i][k];
        }
        for (int row = 0; row < 4; row++)
            for (int k = 0; k < 4; k++) clip[row] += c[row * 4 + k] * v[k];
        if (!isfinite(clip[3]) || clip[3] <= 0) return false;
        const float x = clip[0] / clip[3], y = clip[1] / clip[3];
        if (!isfinite(x) || !isfinite(y) || fabsf(x) < 0.98f || fabsf(y) < 0.98f) return false;
        corners |= 1u << ((x > 0 ? 1 : 0) | (y > 0 ? 2 : 0));
    }
    if (corners != 15) return false;
    for (int i = 0; i < 4; i++) {
        if (!isfinite(c[28 + i]) || c[28 + i] < 0 || c[28 + i] > 1) return false;
        rgba[i] = c[28 + i];
    }
    return true;
}

static inline void vr_fade_over(float premultiplied[4], const float rgba[4]) {
    for (int i = 0; i < 3; i++) premultiplied[i] = rgba[i] * rgba[3] + premultiplied[i] * (1 - rgba[3]);
    premultiplied[3] = rgba[3] + premultiplied[3] * (1 - rgba[3]);
}

typedef struct {
    float blend[4], opaque[4];
    bool opaque_last;
} VrFadeFrame;

static inline void vr_fade_frame_draw(VrFadeFrame *frame) {
    frame->opaque_last = false; // An opaque backdrop followed by content is a panel background, not a final veil.
}

static inline void vr_fade_frame_fill(VrFadeFrame *frame, const float rgba[4], bool blended) {
    if (blended) vr_fade_over(frame->opaque_last ? frame->opaque : frame->blend, rgba);
    else {
        frame->opaque_last = true;
        memset(frame->blend, 0, sizeof frame->blend); // An opaque fill replaces earlier effects.
        memcpy(frame->opaque, rgba, sizeof frame->opaque);
        frame->opaque[3] = 1; // With blending disabled, native coverage does not depend on the stored alpha.
    }
}

static inline bool vr_fade_frame_take(VrFadeFrame *frame, float rgba[4]) {
    memcpy(rgba, frame->opaque_last ? frame->opaque : frame->blend, sizeof frame->blend);
    memset(frame, 0, sizeof *frame);
    return rgba[3] > 0;
}

static inline float vr_fade_srgb_to_linear(float value) {
    return value <= 0.04045f ? value / 12.92f : powf((value + 0.055f) / 1.055f, 2.4f);
}

// Game fades blend display-encoded values; OpenXR blends in linear light. Preserve black-fade transmission
// for a white source (the conservative bound for highlights), rather than letting logos glow through near black.
// Keep the native value for the desktop mirror, whose render target already uses the game's blend space.
static inline void vr_fade_compositor_color(const float native[4], float linear[4]) {
    const float alpha = native[3];
    linear[3] = alpha;
    for (int i = 0; i < 3; i++)
        linear[i] = alpha > 0 ? vr_fade_srgb_to_linear(fminf(1, native[i] / alpha)) * alpha : 0;
    if (native[0] == 0 && native[1] == 0 && native[2] == 0)
        linear[3] = 1 - vr_fade_srgb_to_linear(1 - alpha);
}
