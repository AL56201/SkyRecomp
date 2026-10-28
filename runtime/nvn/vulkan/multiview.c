// VR multiview: a translated guest shader (Vulkan GLSL, maxwell/translate.c) made to render both eyes in one pass.
//
// Each constant buffer cbN and texture tN becomes a two-element descriptor array, one element per eye, indexed by
// gl_ViewIndex: cbN[e] -> cbN_v[gl_ViewIndex].d[e], tN -> tN_v[gl_ViewIndex]. One read, no branch (picking between
// two reads with a ternary cost ~0.9 ms of GPU a frame in the Sealed Temple: the reads weren't skipped). The exec.c
// side binds eye 1's data as element 1 (the same buffer / view when it isn't eye-specific).
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "multiview.h"

typedef struct { char *p; size_t n, cap; bool bad; } Buf;

static void put(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + n + 1) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) { b->bad = true; return; }
        b->p = p, b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void puts_(Buf *b, const char *s) { put(b, s, strlen(s)); }

static bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

// 1: cbN, 2: tN (the whole token), 0: neither
static int eye_token(const char *s, size_t n) {
    const size_t k = s[0] == 't' ? 1 : (n > 2 && s[0] == 'c' && s[1] == 'b') ? 2 : 0;
    if (!k || n <= k) return 0;
    for (size_t i = k; i < n; i++) if (!isdigit((unsigned char)s[i])) return 0;
    return k == 2 ? 1 : 2;
}

static void transform(Buf *out, const char *s, size_t n) {
    size_t i = 0;
    while (i < n && !out->bad) {
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {  // (comments as they are)
            size_t e = i;
            while (e < n && s[e] != '\n') e++;
            put(out, s + i, e - i);
            i = e;
            continue;
        }
        if (!(isalpha((unsigned char)s[i]) || s[i] == '_') || (i && ident_char(s[i - 1]))) {
            put(out, s + i, 1);
            i++;
            continue;
        }
        size_t e = i;
        while (e < n && ident_char(s[e])) e++;
        const int kind = eye_token(s + i, e - i);
        put(out, s + i, e - i);
        if (kind == 1) puts_(out, "_v[gl_ViewIndex].d");
        else if (kind == 2) puts_(out, "_v[gl_ViewIndex]");
        i = e;
    }
}

// A declaration line: "layout(std140, set = S, binding = B) uniform CBN { uvec4 cbN[M]; };" or
// "layout(set = S, binding = B) uniform TYPE tN;" -> the same binding as an array of both eyes'.
static bool declaration(Buf *out, const char *line, size_t n) {
    char text[512];
    if (n >= sizeof text) return false;
    memcpy(text, line, n);
    text[n] = 0;
    int set, binding, idx, idx2, len, used = 0;
    if (sscanf(text, "layout(std140, set = %d, binding = %d) uniform CB%d { uvec4 cb%d[%d]; };%n", &set, &binding, &idx,
               &idx2, &len, &used) == 5 && used == (int)n && idx == idx2) {
        char d[256];
        snprintf(d, sizeof d, "layout(std140, set = %d, binding = %d) uniform CB%d { uvec4 d[%d]; } cb%d_v[2];", set, binding,
                 idx, len, idx);
        puts_(out, d);
        return true;
    }
    char type[64];
    used = 0;
    if (sscanf(text, "layout(set = %d, binding = %d) uniform %63s t%d;%n", &set, &binding, type, &idx, &used) == 4 &&
        used == (int)n && binding >= 32) {
        char d[256];
        snprintf(d, sizeof d, "layout(set = %d, binding = %d) uniform %s t%d_v[2];", set, binding, type, idx);
        puts_(out, d);
        return true;
    }
    return false;
}

char *vk_multiview_glsl(const char *src) {
    if (!src) return NULL;
    Buf out = {0};
    const char *p = src;
    bool version_done = false;
    while (*p && !out.bad) {
        const char *nl = strchr(p, '\n');
        const size_t n = nl ? (size_t)(nl - p) : strlen(p);
        size_t m = n;
        if (m && p[m - 1] == '\r') m--;
        if (p[0] == '#') {
            put(&out, p, n);
            if (!version_done && !strncmp(p, "#version", 8)) {
                puts_(&out, "\n#extension GL_EXT_multiview : require");
                version_done = true;
            }
        } else if (!declaration(&out, p, m)) {
            transform(&out, p, n);
        }
        if (!nl) break;
        put(&out, "\n", 1);
        p = nl + 1;
    }
    if (out.bad || !version_done) { free(out.p); return NULL; }
    return out.p;
}
