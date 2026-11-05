// VR: the sword's hits follow the blade in your hand (VR Options > Sword Hits Follow Your Hand; [VR] sword_hitbox).
//
// The game keeps Link's sword as a swept line inside his actor, at +0x66e4 (his actor: the pointer at main+0x18299d0):
// four world positions - the blade's tip (80 units along the sword model's +X from its hilt), a point near its base
// (12 units along), and both of last frame's. Its hits are taken from that line: moving it 2 m up (SKYRECOMP_SWORD_SHIFT
// below) stops a swing from cutting the grass in front of him. Link's update (main+0xab7cf4) writes it every frame;
// it is called through his vtable, so it can be wrapped (hle_override). Right after it runs, while VR runs in 1st
// person with the right controller tracked and the sword drawn, the line becomes the blade held by the right
// controller: along the grip's forward axis (-z, the way a held sword points), from BASE_AT to TIP_AT game units out,
// this frame's and the last frame's. The game still decides when Link swings (its motion controls); where the blade
// is when it does is now your hand's.
//
// Drawn or sheathed comes from the game's own line: sheathed, its base point sits 19-20 units from Link's eye point
// (main+0x1829a30, runtime/hle/player.c) on his back; drawn, further out in his hand. (Measured in the original
// implementation: the hilt on his back is ~25 units from his head, held 40 or more.)
//
// Found with a memory search for positions that keep their place in the drawn hilt's own frame while the sword swings,
// and the update by watching who writes them (marclemieux64/SkyRecomp PR; first done in a separate VR build:
// "sword hit line"). Diagnostics: SKYRECOMP_SWORD_LOG=1 logs what is written every 120 frames.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../vr/vr_rig.h"
#include "../vr/vr_settings.h"

#define PLAYER_UPDATE (MAIN_BASE + 0xab7cf4)
#define PLAYER_ACTOR_REF (MAIN_BASE + 0x18299d0)
#define PLAYER_EYE (MAIN_BASE + 0x1829a30)
#define SWORD_LINE 0x66e4
#define BASE_AT 12.0f   // game units along the blade from the grip: the game's own base point
#define TIP_AT 80.0f    // ... and its tip
#define DRAWN_AT 30.0f  // the line's base further than this from Link's eye point: drawn (sheathed: 19-20)
#define SHEATHED_AT 25.0f

extern bool vr_running(void);
extern uint64_t renderer_present_count(void);

static GuestFn g_player_update;
static float g_shift[3];
static bool g_shift_on;

static bool sword_drawn(const float *line) {
    static bool drawn;
    static int count;
    const float *e = (const float *)(uintptr_t)PLAYER_EYE;
    const float d[3] = {line[3] - e[0], line[4] - e[1], line[5] - e[2]};
    const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const bool want = drawn ? !(dist < SHEATHED_AT) : dist > DRAWN_AT;  // (a margin, and some patience: a swing
    if (want != drawn) {                                                 // past the head doesn't sheathe it)
        if (++count >= (drawn ? 10 : 3)) drawn = want, count = 0;
    } else count = 0;
    return drawn;
}

static void player_update_hook(Cpu *c) {
    const uint64_t pl = c->x[0];
    g_player_update(c);
    // (only Link himself: the update serves other player-like actors too, the title screen's)
    if (pl != *(const uint64_t *)(uintptr_t)PLAYER_ACTOR_REF) return;
    float *line = (float *)(uintptr_t)(pl + SWORD_LINE);  // tip, base, last tip, last base
    if (g_shift_on) {
        for (int p = 0; p < 4; p++) for (int i = 0; i < 3; i++) line[p * 3 + i] += g_shift[i];
        return;
    }
    static float last[6];
    static bool have_last;
    float grip[12];
    const bool on = g_vr_config.sword_hitbox && g_vr_config.view_mode == 1 && vr_running() && sword_drawn(line) &&
                    vr_rig_controller_pose(1, grip);
    if (!on) { have_last = false; return; }
    float blade[6];  // tip, base: along the grip's -z
    for (int i = 0; i < 3; i++) {
        blade[i] = grip[i * 4 + 3] - grip[i * 4 + 2] * TIP_AT;
        blade[3 + i] = grip[i * 4 + 3] - grip[i * 4 + 2] * BASE_AT;
    }
    if (!have_last) memcpy(last, blade, sizeof last);
    memcpy(line, blade, sizeof blade);
    memcpy(line + 6, last, sizeof last);
    memcpy(last, blade, sizeof last);
    have_last = true;
    static int log = -1;
    static uint64_t logged;
    if (log < 0) log = getenv("SKYRECOMP_SWORD_LOG") != NULL;
    if (log && renderer_present_count() >= logged + 120) {
        logged = renderer_present_count();
        LOG("[sword] hit line from the right controller: tip %.1f %.1f %.1f, base %.1f %.1f %.1f\n", blade[0], blade[1],
            blade[2], blade[3], blade[4], blade[5]);
    }
}

void vr_sword_init(void) {  // (main.c, after the game code is loaded)
    const char *s = getenv("SKYRECOMP_SWORD_SHIFT");  // (test) "x y z": the game's own line moved by that much instead
    g_shift_on = s && sscanf(s, "%f %f %f", &g_shift[0], &g_shift[1], &g_shift[2]) == 3;
    g_player_update = dispatch_lookup(PLAYER_UPDATE);
    if (g_player_update) hle_override(PLAYER_UPDATE, player_update_hook, "vr_sword_player_update");
    LOG("[sword] Link's update %s%s\n", g_player_update ? "hooked" : "not found (the sword's hits stay the game's)",
        g_shift_on ? "; test: his sword's hit line moved" : "");
}
