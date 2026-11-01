#include <assert.h>
#include <stdio.h>
#include "../runtime/vr/vr_connect.h"

int main(void) {
    VrConnectState s = {0};
    assert(!vr_connect_tick(&s, 0, false, false, false));  // launcher / VR disabled
    assert(vr_connect_tick(&s, 1000, true, false, false) == VR_CONNECT_START);
    assert(!vr_connect_tick(&s, 2000, true, false, false));  // failed launch: no repeated spawning
    assert(!vr_connect_tick(&s, 3000, true, false, true));
    assert(!vr_connect_tick(&s, 5000, true, false, true));
    assert(vr_connect_tick(&s, 6000, true, false, true) == VR_CONNECT_TRY);
    assert(!vr_connect_tick(&s, 7000, true, false, true));  // failed OpenXR: back off
    assert(vr_connect_tick(&s, 16000, true, false, true) == VR_CONNECT_TRY);
    assert(!vr_connect_tick(&s, 17000, true, true, true));
    assert(vr_connect_tick(&s, 18000, true, true, false) == VR_CONNECT_STOP);
    assert(!vr_connect_tick(&s, 19000, true, false, false));
    assert(!vr_connect_tick(&s, 26000, true, false, true));  // runtime comes back later
    assert(vr_connect_tick(&s, 29000, true, false, true) == VR_CONNECT_TRY);
    assert(vr_connect_tick(&s, 30000, false, true, true) == VR_CONNECT_STOP);
    assert(vr_connect_tick(&s, 31000, true, false, false) == VR_CONNECT_START);
    puts("VR connection policy: passed");
}
