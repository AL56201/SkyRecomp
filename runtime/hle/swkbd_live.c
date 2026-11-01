// Skyward Sword's name-entry process executes through main+0x23f90 and reports completion through self+0xbc.
// ShowKeyboard is synchronous inside that callback. Keep its native continuation and guest stack separate so
// yielding the dialog returns to the process-tree visitor: the rest of the game can update and render both eyes.
#include "../hle.h"
#include "../vr/vr.h"

#ifdef _WIN32
#include <windows.h>

#define NAME_EXECUTE (MAIN_BASE + 0x23f90)
#define NAME_STACK_SIZE (1024 * 1024)

static GuestFn original_execute;
static struct {
    void *fiber, *parent;
    Cpu cpu;
    uint64_t object, stack;
    DWORD thread;
    bool done, aborted, converted;
} name_task;
static __thread bool in_name_fiber;

static void CALLBACK name_fiber(void *unused) {
    (void)unused;
    original_execute(&name_task.cpu);
    name_task.done = true;
    in_name_fiber = false;
    SwitchToFiber(name_task.parent);
    abort();  // a completed continuation is never resumed
}

static void delete_task(void) {
    if (name_task.fiber) DeleteFiber(name_task.fiber);
    name_task.fiber = NULL;
    name_task.object = 0;
}

// menu_keyboard_run: yield only this callback, without re-presenting a frozen image or touching guest results.
bool swkbd_live_yield(void) {
    if (!in_name_fiber) return false;
    in_name_fiber = false;
    SwitchToFiber(name_task.parent);
    in_name_fiber = true;
    return true;
}

// kernel.c: longjmp must run on the parent fiber containing thread_main's setjmp, never across fiber stacks.
void swkbd_live_unwind(void) {
    if (!in_name_fiber) return;
    name_task.aborted = true;
    in_name_fiber = false;
    SwitchToFiber(name_task.parent);
    abort();
}

static void name_execute(Cpu *c) {
    if (!name_task.fiber && (!vr_requested() || *(const uint8_t *)(uintptr_t)(c->x[0] + 0xbc))) {
        original_execute(c);
        return;
    }
    if (name_task.fiber && (name_task.object != c->x[0] || name_task.thread != GetCurrentThreadId())) {
        // This process is still waiting for its answer. Never resume a Windows fiber on another guest thread.
        c->x[0] = 1;
        return;
    }
    if (!name_task.fiber) {
        name_task.converted = !IsThreadAFiber();
        name_task.parent = name_task.converted ? ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH) : GetCurrentFiber();
        if (!name_task.parent) { original_execute(c); return; }
        if (!name_task.stack) name_task.stack = (uint64_t)(uintptr_t)hle_alloc(NAME_STACK_SIZE);
        name_task.cpu = *c;
        name_task.cpu.sp = (name_task.stack + NAME_STACK_SIZE - 16) & ~15ull;
        name_task.object = c->x[0];
        name_task.thread = GetCurrentThreadId();
        name_task.done = name_task.aborted = false;
        name_task.fiber = CreateFiberEx(64 * 1024, 4 * 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, name_fiber, NULL);
        if (!name_task.fiber) {
            if (name_task.converted) ConvertFiberToThread();
            original_execute(c);
            return;
        }
    }
    in_name_fiber = true;
    SwitchToFiber(name_task.fiber);
    in_name_fiber = false;
    if (name_task.done || name_task.aborted) {
        const bool aborted = name_task.aborted, converted = name_task.converted;
        c->x[0] = name_task.cpu.x[0];
        delete_task();
        if (converted) ConvertFiberToThread();
        if (aborted) rt_unwind_point();
    } else {
        // The original callback always returns 1. Its completion byte remains zero until ShowKeyboard resumes.
        c->x[0] = 1;
    }
}

void swkbd_live_init(void) {
    if (!g_is_skyward) return;
    original_execute = dispatch_lookup(NAME_EXECUTE);
    if (original_execute) {
        hle_override(NAME_EXECUTE, name_execute, "live_name_entry");
        LOG("[swkbd] live name entry: game updates and stereo rendering continue while typing\n");
    }
}

RT_ON_RESET(swkbd_live_reset) {
    // Guest threads have stopped; any suspended continuation is no longer reachable and guest memory is wiped.
    delete_task();
    name_task.stack = 0;
    original_execute = NULL;
}
#else
bool swkbd_live_yield(void) { return false; }
void swkbd_live_unwind(void) {}
void swkbd_live_init(void) {}
#endif
