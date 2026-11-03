// Asynchronous command replay: the game thread hands its submitted command lists, presents and texture releases to
// one replay thread, in order, and goes on building the next frame while the previous one is translated to Vulkan
// and shown. On the Switch the GPU works the same way, behind the CPU; the game already fences what it reuses.
//
// What crosses threads:
//  - fences: a sync gets the queue position (ticket) of the item that signals it (nvnQueueFenceSync, or a list
//    that holds a CMD_FENCE_SYNC, found when the list is submitted); nvnSyncWait waits for the replay thread to pass
//    that ticket, then for the GPU submission it recorded. nvnQueueFinish drains the queue.
//  - pacing: at most one frame queued ahead (a present waits for the previous present to be done), so input latency
//    grows by one frame at most and the game is paced by the replay thread's own pacing (xrWaitFrame in VR).
//  - game memory the draws interpret (the VR rig's camera and Link's pose, hle/player.c) is snapshotted with each
//    list at submission; the replay thread reads the snapshot.
//  - the per-frame hooks that touch the game (input, warp, tests) stay on the game thread (device.c
//    present_game_hooks), run when the present is queued.
//
// Vulkan only (not the Metal backend). On by default, in and out of VR: Display > Multithreaded Rendering
// (g_settings.async_replay, read at startup); SKYRECOMP_ASYNC_REPLAY=0 / 1 overrides. Desktop, Skyloft at 144 FPS
// (2026-10-26): ~108-120 fps without it, a steady 144 with it; images the same apart from animation timing.
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../kernel.h"
#include "../settings.h"
#include "nvn.h"


void backend_execute(uint64_t handle);
void backend_submit_commands(void);
void backend_present(NvnTexture *tex);
void backend_represent(void);
void backend_texture_destroyed(NvnTexture *t);
uint64_t renderer_fence_seq(void);
void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
void present_game_hooks(uint64_t frame);
size_t player_snapshot_size(void);
void player_snapshot_take(void *out);
void player_snapshot_use(const void *snap);
extern volatile int g_rt_unwind;
extern __thread uint64_t g_blocked_ns;

#ifdef __APPLE__
bool nvn_async_on(void) { return false; }
uint64_t nvn_async_game_frame(void) { return 0; }
bool nvn_async_is_replay_thread(void) { return false; }
uint64_t nvn_async_idle_ns(void) { return 0; }
void nvn_async_execute(uint64_t handle) { (void)handle; }
void nvn_async_submit(void) {}
void nvn_async_present(NvnTexture *t) { (void)t; }
void nvn_async_represent(void) {}
void nvn_async_texture_finalize(NvnTexture *t) { (void)t; }
void nvn_async_queue_fence(uint64_t sync) { (void)sync; }
void nvn_async_wait_sync(uint64_t sync) { (void)sync; }
void nvn_async_drain(void) {}
void nvn_async_stop(void) {}
#else
enum { IT_EXEC, IT_SUBMIT, IT_PRESENT, IT_REPRESENT, IT_TEX_FINALIZE, IT_FENCE, IT_STOP };
typedef struct {
    int kind;
    uint64_t a;
    void *p;
    uint8_t snap[96];
} Item;

#define QCAP 1024
static Item q[QCAP];
static uint64_t q_enq, q_done;            // items queued / finished (tickets)
static uint64_t presents_enq, presents_done;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_work = PTHREAD_COND_INITIALIZER, q_progress = PTHREAD_COND_INITIALIZER;
static pthread_t q_thread;
// SKYRECOMP_ASYNC_TRACE=1: every 300 presents, where each thread waited for the other
static uint64_t st_replay_idle, st_game_backpressure, st_game_sync, st_items;
static uint64_t cum_replay_idle;  // (never reset: vr.c's per-frame pacing trace)
uint64_t nvn_async_idle_ns(void) { return cum_replay_idle; }
static bool q_running;

bool nvn_async_on(void) {
    {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SKYRECOMP_ASYNC_REPLAY");  // (default: the setting, on)
        on = e ? atoi(e) != 0 : g_settings.async_replay;
        if (player_snapshot_size() > sizeof q[0].snap) on = 0;
        if (on) LOG("[nvn] asynchronous command replay on\n");
    }
    return on;
    }
}

static __thread bool tl_replay;
bool nvn_async_is_replay_thread(void) { return tl_replay; }

// (VR menu pointer, game thread) The frames the game has presented so far: the one it is building is the next.
uint64_t nvn_async_game_frame(void) {
    pthread_mutex_lock(&q_lock);
    const uint64_t n = presents_enq;
    pthread_mutex_unlock(&q_lock);
    return n;
}

static void *replay_main(void *arg) {
    (void)arg;
    tl_replay = true;
    for (;;) {
        pthread_mutex_lock(&q_lock);
        const uint64_t idle0 = plat_now_ns();
        while (q_done == q_enq) pthread_cond_wait(&q_work, &q_lock);
        st_replay_idle += plat_now_ns() - idle0;
        cum_replay_idle += plat_now_ns() - idle0;
        st_items++;
        Item it = q[q_done % QCAP];
        pthread_mutex_unlock(&q_lock);
        switch (it.kind) {
        case IT_EXEC:
            player_snapshot_use(it.snap);
            backend_execute(it.a);
            player_snapshot_use(NULL);
            break;
        case IT_SUBMIT: backend_submit_commands(); break;
        case IT_PRESENT: backend_present(it.p); break;
        case IT_REPRESENT: backend_represent(); break;
        case IT_TEX_FINALIZE: backend_texture_destroyed(it.p); free(it.p); break;
        case IT_FENCE: nvn_sync_set_seq(it.a, renderer_fence_seq()); break;
        default: break;
        }
        pthread_mutex_lock(&q_lock);
        q_done++;
        if (it.kind == IT_PRESENT || it.kind == IT_REPRESENT) presents_done++;
        pthread_cond_broadcast(&q_progress);
        const bool stop = it.kind == IT_STOP;
        pthread_mutex_unlock(&q_lock);
        if (stop) return NULL;
    }
}

// (q_lock held) Waits for the replay thread to finish ticket t. A reboot (g_rt_unwind) gives up: the game thread
// unwinds at its next kernel or HLE call.
static void wait_done_locked(uint64_t t) {
    const uint64_t t0 = plat_now_ns();
    while (q_done < t && !g_rt_unwind) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 2000000;
        if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
        pthread_cond_timedwait(&q_progress, &q_lock, &ts);
    }
    g_blocked_ns += plat_now_ns() - t0;  // (pacing statistics: waiting, not work)
}

static void mark_fences(uint64_t handle, uint64_t ticket, int depth);

// Queues an item; returns its ticket (done once q_done >= ticket). A list's fences get its ticket in the same step.
static uint64_t enqueue(int kind, uint64_t a, void *p, bool snapshot) {
    pthread_mutex_lock(&q_lock);
    if (!q_running) {
        q_running = true;
        pthread_create(&q_thread, NULL, replay_main, NULL);
    }
    while (q_enq - q_done >= QCAP) pthread_cond_wait(&q_progress, &q_lock);  // (the replay thread always progresses)
    Item *it = &q[q_enq % QCAP];
    it->kind = kind;
    it->a = a;
    it->p = p;
    if (snapshot) player_snapshot_take(it->snap);
    if (kind == IT_EXEC) mark_fences(a, q_enq + 1, 0);
    if (kind == IT_FENCE) ((NvnSync *)p)->ticket = q_enq + 1;
    const uint64_t ticket = ++q_enq;
    pthread_cond_signal(&q_work);
    pthread_mutex_unlock(&q_lock);
    return ticket;
}

static NvnSync *sync_host(uint64_t guest) {
    NvnSync *s = HOST(NvnSync, guest);
    if (!s) { s = calloc(1, sizeof *s); SET_HOST(guest, s); }
    return s;
}

// The syncs a list signals (CMD_FENCE_SYNC, nested lists included) belong to its ticket.
static void mark_fences(uint64_t handle, uint64_t ticket, int depth) {
    uint64_t p = handle;
    for (int guard = 0; guard < 10000000 && p; guard++) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        if (r->op == CMD_END) return;
        if (r->op == CMD_JUMP) { p = r->x[0]; continue; }
        if (r->op == CMD_CALL && depth < 8) {
            const uint64_t *h = (const uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[0]; i++) mark_fences(h[i], ticket, depth + 1);
        } else if (r->op == CMD_FENCE_SYNC && r->x[1]) {
            NvnSync *s = sync_host(r->x[1]);
            s->ticket = ticket;
        }
        if (!r->size) return;
        p += r->size;
    }
}

// ---- the game thread's side (nvn/objects.c)
void nvn_async_execute(uint64_t handle) { enqueue(IT_EXEC, handle, NULL, true); }

void nvn_async_submit(void) { enqueue(IT_SUBMIT, 0, NULL, false); }

static void present_queued(void) {
    pthread_mutex_lock(&q_lock);
    const uint64_t n = ++presents_enq;
    pthread_mutex_unlock(&q_lock);
    present_game_hooks(n);
    // One frame ahead at most: the previous present must be done before this frame's work goes on.
    pthread_mutex_lock(&q_lock);
    const uint64_t t0 = plat_now_ns();
    while (presents_done + 1 < n && !g_rt_unwind) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 2000000;
        if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
        pthread_cond_timedwait(&q_progress, &q_lock, &ts);
    }
    g_blocked_ns += plat_now_ns() - t0;
    st_game_backpressure += plat_now_ns() - t0;
    static int trace = -1;
    if (trace < 0) trace = getenv("SKYRECOMP_ASYNC_TRACE") != NULL;
    if (trace && n % 300 == 0) {
        LOG("[nvn] async per frame: replay thread idle %.2f ms, game waits: next frame %.2f ms, syncs %.2f ms; %.0f items\n",
            st_replay_idle / 300e6, st_game_backpressure / 300e6, st_game_sync / 300e6, st_items / 300.0);
        st_replay_idle = st_game_backpressure = st_game_sync = st_items = 0;
    }
    pthread_mutex_unlock(&q_lock);
}

void nvn_async_present(NvnTexture *t) {
    enqueue(IT_PRESENT, 0, t, false);
    present_queued();
}

void nvn_async_represent(void) {
    enqueue(IT_REPRESENT, 0, NULL, false);
    present_queued();
}

void nvn_async_texture_finalize(NvnTexture *t) { enqueue(IT_TEX_FINALIZE, 0, t, false); }

void nvn_async_queue_fence(uint64_t sync) {
    NvnSync *s = sync_host(sync);
    s->signaled = 1;
    enqueue(IT_FENCE, sync, s, false);
}

// nvnSyncWait: the replay thread has passed the sync's signalling item (its GPU submission is then known).
void nvn_async_wait_sync(uint64_t sync) {
    const uint64_t t = sync_host(sync)->ticket;
    pthread_mutex_lock(&q_lock);
    const uint64_t t0 = plat_now_ns();
    if (t > q_done) wait_done_locked(t);
    st_game_sync += plat_now_ns() - t0;
    pthread_mutex_unlock(&q_lock);
}

// Everything queued so far is done (nvnQueueFinish, before a full sync).
void nvn_async_drain(void) {
    if (!nvn_async_on()) return;
    pthread_mutex_lock(&q_lock);
    wait_done_locked(q_enq);
    pthread_mutex_unlock(&q_lock);
}

// (teardown, main thread, guest threads stopped) Finishes what is queued and ends the replay thread.
void nvn_async_stop(void) {
    pthread_mutex_lock(&q_lock);
    const bool running = q_running;
    pthread_mutex_unlock(&q_lock);
    if (!running) return;
    enqueue(IT_STOP, 0, NULL, false);
    pthread_join(q_thread, NULL);
    pthread_mutex_lock(&q_lock);
    q_running = false;
    q_enq = q_done = presents_enq = presents_done = 0;
    pthread_mutex_unlock(&q_lock);
}
#endif  // !__APPLE__
