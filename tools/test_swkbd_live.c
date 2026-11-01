// Actual Windows fiber scheduling: pending name entry must leave the frame caller and its guest stack usable.
#include <assert.h>
#include <setjmp.h>
#include "../runtime/hle/swkbd_live.c"

bool g_is_skyward = true;
volatile int g_rt_unwind;
static bool vr_on = true, answer;
static unsigned background_frames, completed;
static GuestFn installed;
static jmp_buf unwind_test;
bool vr_requested(void) { return vr_on; }
void *hle_alloc(uint64_t n) { return calloc(1, n); }
void rt_unwind_point(void) { swkbd_live_unwind(); longjmp(unwind_test, 1); }

static void fake_name_execute(Cpu *c) {
    uint64_t *local = (uint64_t *)(uintptr_t)(c->sp - 64);
    *local = 0x123456789abcdef0ull;
    while (!answer) {
        if (g_rt_unwind) rt_unwind_point();
        assert(swkbd_live_yield());
        assert(*local == 0x123456789abcdef0ull);
    }
    completed++;
    *(uint8_t *)(uintptr_t)(c->x[0] + 0xbc) = 1;
    c->x[0] = 1;
}
GuestFn dispatch_lookup(uint64_t pc) { assert(pc == NAME_EXECUTE); return fake_name_execute; }
void hle_override(uint64_t pc, HostFn fn, const char *name) { assert(pc == NAME_EXECUTE && name); installed = fn; }

int main(void) {
    unsigned char process[256] = {0}, parent_stack[4096];
    Cpu c = {0};
    c.sp = (uintptr_t)(parent_stack + sizeof parent_stack - 16);
    swkbd_live_init();
    for (unsigned frame = 0; frame < 20; frame++) {
        memset(parent_stack, frame, sizeof parent_stack);  // parent freely reuses its stack between suspended calls
        c.x[0] = (uintptr_t)process;
        c.x[19] = 0xfeed;
        installed(&c);
        assert(c.x[0] == 1 && c.x[19] == 0xfeed);
        assert(!process[0xbc] && !completed);
        assert(parent_stack[0] == frame);
        background_frames++;
    }
    answer = true;
    c.x[0] = (uintptr_t)process;
    installed(&c);
    assert(background_frames == 20 && completed == 1 && process[0xbc]);
    assert(!name_task.fiber && !IsThreadAFiber());
    process[0xbc] = 0;
    answer = false;
    c.x[0] = (uintptr_t)process;
    installed(&c);
    g_rt_unwind = 1;
    if (!setjmp(unwind_test)) { c.x[0] = (uintptr_t)process; installed(&c); assert(0); }
    assert(!name_task.fiber && !IsThreadAFiber() && completed == 1);
    g_rt_unwind = 0;
    vr_on = false; answer = true;
    c.x[0] = (uintptr_t)process;
    installed(&c);
    assert(completed == 2 && !name_task.fiber && !IsThreadAFiber());  // desktop keeps the synchronous ABI
    swkbd_live_reset();
    puts("live name entry: background frames, isolated guest stack, completion and reboot cancellation passed");
}
