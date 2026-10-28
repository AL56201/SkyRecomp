// VR: the 3D world keeps being drawn while a full-screen menu (the map) pauses the game.
//
// On a flat screen the map covers everything, so the game stops drawing the world behind it; in a headset the world
// vanished around the player. Three things stop it (found 2026-10-25: draw-call chains, function profiles, snapshot
// diffs and watchpoints; docs/vr.md):
//  1. The map's "enter full screen" state callback (main+0xbce320) sets the pending draw mask (|= 0x10): skipped.
//  2. preDraw (vtable +0x38, called by the process-tree visitor main+0xf0fb3c) refuses while the draw mask
//     (main+0x1827374, copied each frame from main+0x1827378 by main+0xb9a3e4) is set and the node's properties don't
//     match it: the preDraws that read it run with it cleared (for menus that set it some other way).
//  3. The world's objects enter the 3D scene root each frame during their own execute (logic) phase, which a paused
//     game skips (preExecute main+0xb175c0 refuses while the execute mask main+0x1827370 is set): the scene is empty.
//     The objects still exist, frozen: the root's children of the last unpaused frame are re-entered (the root's own
//     insert) at the start of each paused frame's execute phase. Nothing runs that would move the game on.
// The renderer then shows the menu's 2D layer as an opaque panel in front of the live world (nvn/vulkan/exec.c).
//
// On with VR and the HUD panel (both by default) and VR Options > World Behind Menus; SKYRECOMP_VR_WORLD_IN_MENUS=0
// turns it off, =1 forces it on.
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"
#include "../vr/vr.h"

#define DRAW_MASK (MAIN_BASE + 0x1827374)
#define EXEC_MASK (MAIN_BASE + 0x1827370)  // (the execute phase's: set while a menu pauses the game)

extern uint64_t renderer_present_count(void);

static bool world_in_menus(void) {
    static int env = -2, panel = -1;  // (env: -1 unset, else forced 0 / 1)
    if (env < -1) {
        const char *e = getenv("SKYRECOMP_VR_WORLD_IN_MENUS");
        env = e ? atoi(e) != 0 : -1;
        extern bool vk_hud_layer_enabled(void);  // (the panel: on by default, SKYRECOMP_VR_HUD_LAYER=0)
        panel = vk_hud_layer_enabled();
    }
    // (VR Options > World Behind Menus, read live: it takes effect from the next menu opened)
    const bool on = env >= 0 ? env : panel && g_settings.vr_world_in_menus;
    return on && vr_running();
}

// ---- the scene root: global at main+0x2bfe688; children: array +0xf0, capacity +0xf8, count +0xfc
#define SCENE_ROOT_PTR (MAIN_BASE + 0x2bfe688)
#define SCENE_ROOT_VTABLE 0x7001699050ull
enum { MAX_CHILDREN = 1024 };
static uint64_t g_snap[MAX_CHILDREN], g_snap_vt[MAX_CHILDREN];  // (children; their vtables when taken)
static uint32_t g_nsnap;
static uint64_t g_snap_frame = ~0ull, g_replay_frame = ~0ull;

static uint64_t scene_root(void) {
    const uint64_t r = *(const uint64_t *)(uintptr_t)SCENE_ROOT_PTR;
    return r >= 0x7000000000ull && r < 0x7800000000ull && *(const uint64_t *)(uintptr_t)r == SCENE_ROOT_VTABLE ? r : 0;
}

// (execute phase of an unpaused frame) The list fills during the phase and is emptied before the draw: the fullest
// list of the frame is kept.
static void scene_snapshot(void) {
    const uint64_t f = renderer_present_count(), r = scene_root();
    if (!r) return;
    if (f != g_snap_frame) g_snap_frame = f, g_nsnap = 0;
    const uint64_t *list = *(uint64_t *const *)(uintptr_t)(r + 0xf0);
    const uint32_t n = *(const uint32_t *)(uintptr_t)(r + 0xfc);
    if (!list || n > MAX_CHILDREN || n <= g_nsnap) return;
    memcpy(g_snap, list, n * sizeof *g_snap);
    for (uint32_t i = 0; i < n; i++) g_snap_vt[i] = g_snap[i] ? *(const uint64_t *)(uintptr_t)g_snap[i] : 0;
    g_nsnap = n;
}

// (execute phase of a paused frame, once) The last unpaused frame's children go back in.
static void scene_replay(Cpu *c) {
    const uint64_t f = renderer_present_count(), r = scene_root();
    if (f == g_replay_frame || !r || !g_nsnap) return;
    g_replay_frame = f;
    const uint64_t insert = *(const uint64_t *)(uintptr_t)(*(const uint64_t *)(uintptr_t)r + 0x60);  // (main+0xfecfe4)
    uint64_t saved[31];
    memcpy(saved, c->x, sizeof saved);
    int added = 0;
    for (uint32_t i = 0; i < g_nsnap; i++) {
        const uint64_t *list = *(uint64_t *const *)(uintptr_t)(r + 0xf0);
        const uint32_t n = *(const uint32_t *)(uintptr_t)(r + 0xfc);
        bool present = false;
        for (uint32_t k = 0; k < n && !present; k++) present = list[k] == g_snap[i];
        if (present) continue;
        // (an object deleted since - an arrow that landed while the item wheel was open... - is freed memory:
        // handing it back made the game abort; its vtable no longer matches)
        if (!g_snap[i] || *(const uint64_t *)(uintptr_t)g_snap[i] != g_snap_vt[i]) continue;
        c->x[0] = r;  // insert(root, index, child) -> bool: refuses a child that has a parent (+8) or a full list
        c->x[1] = n;
        c->x[2] = g_snap[i];
        guest_call(c, insert);
        added += (uint32_t)c->x[0] & 1;
    }
    memcpy(c->x, saved, sizeof saved);
    static int logged;
    if (!logged++) LOG("[vr-world] menu: %d frozen world objects re-entered into the scene each frame\n", added);
}

// A menu pauses the game and the frozen world is re-entered (this frame or the last).
bool vr_world_menu_active(void) {
    return g_replay_frame != ~0ull && g_replay_frame + 1 >= renderer_present_count() &&
           *(volatile uint32_t *)(uintptr_t)EXEC_MASK && world_in_menus();
}

#define PRE_EXECUTE 0x7000b175c0ull
static GuestFn g_orig_pre_execute;
static void pre_execute(Cpu *c) {
    if (world_in_menus()) {
        if (!*(volatile uint32_t *)(uintptr_t)EXEC_MASK) scene_snapshot();
        else scene_replay(c);
    }
    g_orig_pre_execute(c);
}

// preDraw implementations reading the draw mask (watchpoint), called through vtables (so through the dispatcher,
// where overrides apply): the base node's, the world (stage) node's, and the actor base's.
static const uint64_t k_pre_draw[] = {0x7000ec620cull, 0x7000aaf4acull, 0x7000b1756cull};
static GuestFn g_orig[sizeof k_pre_draw / sizeof k_pre_draw[0]];
static void pre_draw(Cpu *c, int i) {
    volatile uint32_t *mask = (volatile uint32_t *)(uintptr_t)DRAW_MASK;
    const uint32_t saved = *mask;
    const bool clear = saved && world_in_menus();
    if (clear) *mask = 0;
    g_orig[i](c);
    if (clear) *mask = saved;
}
static void pre_draw0(Cpu *c) { pre_draw(c, 0); }
static void pre_draw1(Cpu *c) { pre_draw(c, 1); }
static void pre_draw2(Cpu *c) { pre_draw(c, 2); }

// The map's "enter full screen" state callback (called once through a state table when the map opens): pending
// draw mask |= 0x10, and [[main+0x183c9a0] + 0x2188c] = 0.
#define MAP_DRAW_OFF 0x7000bce320ull
static GuestFn g_orig_map_draw_off;
static void map_draw_off(Cpu *c) {
    if (world_in_menus()) return;
    g_orig_map_draw_off(c);
}

// (main.c, after dispatch_init and before the game runs: the dispatcher's caches must not hold these yet)
void vr_world_init(void) {
    int n = 0;
    if ((g_orig_pre_execute = dispatch_lookup(PRE_EXECUTE))) hle_override(PRE_EXECUTE, pre_execute, "vr_world_pre_execute"), n++;
    if ((g_orig_map_draw_off = dispatch_lookup(MAP_DRAW_OFF))) hle_override(MAP_DRAW_OFF, map_draw_off, "vr_world_map_draw_off"), n++;
    static const HostFn wrap[] = {pre_draw0, pre_draw1, pre_draw2};
    for (size_t i = 0; i < sizeof k_pre_draw / sizeof k_pre_draw[0]; i++) {
        g_orig[i] = dispatch_lookup(k_pre_draw[i]);
        if (g_orig[i]) hle_override(k_pre_draw[i], wrap[i], "vr_world_pre_draw"), n++;
    }
    LOG("[vr-world] %d hooks (the world behind menus: with VR and the HUD panel)\n", n);
}
