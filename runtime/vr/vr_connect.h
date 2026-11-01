#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { VR_CONNECT_START = 1, VR_CONNECT_TRY = 2, VR_CONNECT_STOP = 4 };
typedef struct { bool launched, ready_seen; uint64_t ready_since, retry_at; } VrConnectState;

// Called at most once a second while enabled. Wait for a stable compositor before entering OpenXR;
// a failed connection retries slowly, without repeatedly launching SteamVR.
static inline unsigned vr_connect_tick(VrConnectState *s, uint64_t now, bool enabled, bool connected, bool ready) {
    if (!enabled) {
        *s = (VrConnectState){0};
        return connected ? VR_CONNECT_STOP : 0;
    }
    if (!ready) {
        s->ready_seen = false;
        unsigned action = connected ? VR_CONNECT_STOP : 0;
        if (!s->launched) { s->launched = true; action |= VR_CONNECT_START; }
        return action;
    }
    if (connected) return 0;
    if (!s->ready_seen) { s->ready_seen = true; s->ready_since = now; }
    if (now - s->ready_since < 3000 || now < s->retry_at) return 0;
    s->retry_at = now + 10000;
    return VR_CONNECT_TRY;
}
