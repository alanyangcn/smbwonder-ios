// Horizon thread and synchronization SVCs for Wonder: each guest thread is one host pthread running on
// the guest stack the SDK supplies; nn::os runs unchanged above this "kernel".
//   threads 0x08 Create 0x09 Start 0x0A Exit 0x0B Sleep 0x0C/0x0D priority 0x0E/0x0F core mask
//           0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
//   locks   0x1A ArbitrateLock 0x1B ArbitrateUnlock (tag word = owner handle | 0x40000000 waiters)
//   condvar 0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   address 0x34 WaitForAddress 0x35 SignalToAddress
//   events  0x45 CreateEvent 0x11 Signal 0x12 Clear 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
// Semantics: public Horizon kernel behaviour (Atmosphere mesosphere KConditionVariable / KAddressArbiter /
// KSynchronization). Ported from D2R dm/host/hle_thread.c (real-device tested); here invalid handles and
// non-guest callers are errors, never silent successes. One global lock G, one condvar per thread.
#include "kernel_events.h"
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif
#define R_TIMEOUT 0xEA01u
#define R_CANCELLED 0xEC01u
#define R_BAD_HANDLE 0xE401u
#define R_BAD_STATE 0xFA01u
#define R_OUT_OF_RESOURCE 0xCE01u
#define R_INVALID_PRIORITY 0xE001u
#define R_INVALID_CORE 0xE201u
#define R_INVALID_COMBINATION 0xE801u
#define R_INVALID_POINTER 0xCC01u
#define R_OUT_OF_RANGE 0xE601u
#define WAITERS_BIT 0x40000000u
#define H_CUR_THREAD 0xFFFF8000u
#define MAX_H 1024
#define PROCESS_CORE_MASK 7u          // application cores 0-2 (GetInfo id 0)

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W };
enum { W_NONE, W_LOCK, W_CONDVAR, W_ADDRESS, W_SYNC };
struct event { int signaled, refs; };
struct thr {
    uint32_t handle; uint64_t id, entry, arg, stack_top; int prio, core; uint64_t mask;
    int started, exited, cancel, closed, guest;
    pthread_t pt; pthread_cond_t cv;
    jmp_buf host_exit; int host_exit_ready;
    int wkind, woke; uint64_t waddr, wkey, wseq; uint32_t wtag; int64_t wres;
    struct thr *next, *all;
};
static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
static struct { int type; void *obj; } handles[MAX_H];
static pthread_key_t key_thread; static pthread_once_t key_once = PTHREAD_ONCE_INIT;
static uint64_t next_id = 0x51, wait_seq;
static struct thr *waiting, *all_threads;
static void (*prepare_hook)(void);
static void (*enter_hook)(uint64_t entry, uint64_t arg, uint64_t sp);
static void (*report_hook)(const char *line);
static void report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void report(const char *fmt, ...) {
    if (!report_hook) return;
    char line[256]; va_list a; va_start(a, fmt); vsnprintf(line, sizeof line, fmt, a); va_end(a); report_hook(line);
}
static void make_key(void) { pthread_key_create(&key_thread, NULL); }

static uint32_t h_new(int type, void *obj) {   // holds G
    for (int i = 1; i < MAX_H; i++) if (!handles[i].type) { handles[i].type = type; handles[i].obj = obj; return 0x1000u + (uint32_t)i; }
    return 0;
}
static struct thr *self(void) { pthread_once(&key_once, make_key); return pthread_getspecific(key_thread); }
static void *h_peek(uint32_t h, int type) {
    if (h == H_CUR_THREAD && type == O_THREAD) return self();
    unsigned i = h - 0x1000u;
    if (i == 0 || i >= MAX_H || handles[i].type != type) return NULL;
    return handles[i].obj;
}
static int owned(uint32_t h) { unsigned i = h - 0x1000u; return h == H_CUR_THREAD || (i > 0 && i < MAX_H && handles[i].type); }

// ---------- waits (hold G) ----------
static void unlink_wait(struct thr *t) {
    for (struct thr **p = &waiting; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    t->next = NULL;
}
static struct thr *pick(int kind, uint64_t a) {   // kernel order: lower priority value first, then FIFO
    struct thr *b = NULL;
    for (struct thr *t = waiting; t; t = t->next) {
        if (t->wkind != kind || (kind == W_CONDVAR ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}
static void wake(struct thr *t, int64_t res) { unlink_wait(t); t->wkind = W_NONE; t->woke = 1; t->wres = res; pthread_cond_signal(&t->cv); }
static int64_t block(struct thr *t, int kind, int64_t ns) {   // ns<0 forever, 0 immediate timeout
    t->wkind = kind; t->woke = 0; t->next = waiting; waiting = t; t->wseq = ++wait_seq;
    if (ns == 0) { unlink_wait(t); t->wkind = W_NONE; return R_TIMEOUT; }
    struct timespec dl;
    if (ns > 0) { clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += ns / 1000000000; dl.tv_nsec += ns % 1000000000;
                  if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; } }
    while (!t->woke) {
        int rc = ns > 0 ? pthread_cond_timedwait(&t->cv, &G, &dl) : pthread_cond_wait(&t->cv, &G);
        if (rc == ETIMEDOUT && !t->woke) { unlink_wait(t); t->wkind = W_NONE; return R_TIMEOUT; }
    }
    return t->wres;
}
static void wake_sync_waiters(void) {
    for (struct thr *w = waiting; w; ) { struct thr *n = w->next; if (w->wkind == W_SYNC) wake(w, -1); w = n; }
}

// ---------- locks ----------
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *e, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)a, e, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
static void lock_handoff(uint64_t addr) {
    struct thr *w = pick(W_LOCK, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (struct thr *t = waiting; t; t = t->next) if (t != w && t->wkind == W_LOCK && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
static void lock_acquire_for(struct thr *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = W_LOCK; t->waddr = addr; t->wtag = tag; t->wseq = ++wait_seq;
}

// ---------- threads ----------
static void apply_priority(int prio) {   // only the calling thread's QoS can be changed on Darwin
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(prio < 16 ? QOS_CLASS_USER_INTERACTIVE : prio < 48 ? QOS_CLASS_USER_INITIATED : QOS_CLASS_UTILITY, 0);
#else
    (void)prio;
#endif
}
static void finish_exit_on_host(struct thr *t) __attribute__((noreturn));
static void *thread_main(void *p) {
    struct thr *t = p;
    pthread_setspecific(key_thread, t);
    if (prepare_hook) prepare_hook();   // guest TLS / TLR / gate keys
    pthread_mutex_lock(&G);
    while (!t->started && !t->closed) pthread_cond_wait(&t->cv, &G);
    int abandoned = !t->started;
    pthread_mutex_unlock(&G);
    if (abandoned) finish_exit_on_host(t);
    apply_priority(t->prio);
    // This frame remains on the pthread's host stack while enter_hook switches to the guest stack.
    // ExitThread must restore it BEFORE waking any waiter allowed to recycle the guest stack.
    if (!setjmp(t->host_exit)) {
        t->host_exit_ready = 1;
        enter_hook(t->entry, t->arg, t->stack_top);
        abort();
    }
    finish_exit_on_host(t);
}
static void free_thread(struct thr *t) {   // holds G
    for (struct thr **p = &all_threads; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    pthread_cond_destroy(&t->cv); free(t);
}
static struct thr *new_thread(void) {
    struct thr *t = calloc(1, sizeof *t);
    if (t) pthread_cond_init(&t->cv, NULL);
    return t;
}
uint32_t wonder_threads_register_current(int prio, int core, void (*prepare)(void), void (*enter)(uint64_t, uint64_t, uint64_t),
                                         void (*reporter)(const char *)) {
    pthread_once(&key_once, make_key);
    if (prepare) prepare_hook = prepare;
    if (enter) enter_hook = enter;
    if (reporter) report_hook = reporter;
    struct thr *t = new_thread();
    if (!t) return 0;
    t->started = 1; t->guest = 1; t->prio = prio; t->core = core; t->mask = 1ull << core; t->pt = pthread_self();
    pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = next_id++; t->all = all_threads; all_threads = t; pthread_mutex_unlock(&G);
    pthread_setspecific(key_thread, t);
    return t->handle;
}
static uint32_t create_thread(uint64_t *r) {
    int prio = (int)r[4], core = (int)r[5];
    if (prio < 0 || prio > 63) return R_INVALID_PRIORITY;
    if (core == -2) core = 0;                                  // process default (ideal core 0)
    if (core < 0 || core > 2 || !(PROCESS_CORE_MASK >> core & 1)) return R_INVALID_CORE;
    if (!r[1] || (r[3] & 0xf) || !enter_hook) return R_INVALID_POINTER;
    struct thr *t = new_thread();
    if (!t) return R_OUT_OF_RESOURCE;
    t->entry = r[1]; t->arg = r[2]; t->stack_top = r[3]; t->prio = prio; t->core = core; t->mask = 1ull << core; t->guest = 1;
    pthread_mutex_lock(&G);
    t->handle = h_new(O_THREAD, t);
    if (!t->handle) { pthread_mutex_unlock(&G); pthread_cond_destroy(&t->cv); free(t); return R_OUT_OF_RESOURCE; }
    t->id = next_id++; t->all = all_threads; all_threads = t;
    pthread_mutex_unlock(&G);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 512 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);   // the guest waits on the handle, never joins
    int error = pthread_create(&t->pt, &a, thread_main, t);
    pthread_attr_destroy(&a);
    if (error) { pthread_mutex_lock(&G); handles[t->handle - 0x1000u].type = O_FREE; free_thread(t); pthread_mutex_unlock(&G); return R_OUT_OF_RESOURCE; }
    report("HOST_THREAD_CREATE handle=%#x id=%#llx entry=%#llx arg=%#llx sp=%#llx prio=%d core=%d", t->handle,
           (unsigned long long)t->id, (unsigned long long)t->entry, (unsigned long long)t->arg, (unsigned long long)t->stack_top, prio, core);
    r[1] = t->handle;
    return 0;
}
static void finish_exit_on_host(struct thr *t) {
    pthread_mutex_lock(&G);
    t->exited = 1;
    wake_sync_waiters();
    pthread_setspecific(key_thread, NULL);
    if (t->closed) free_thread(t);
    pthread_mutex_unlock(&G);
    pthread_exit(NULL);
}
static void exit_thread(struct thr *t) {
    if (t->host_exit_ready) longjmp(t->host_exit, 1);
    // Registered main thread already runs on its pthread-owned NPDM stack, not a recyclable MapMemory
    // worker stack. Never establish setjmp in register_current: that frame has already returned.
    finish_exit_on_host(t);
}

// ---------- synchronization objects ----------
static int signaled(uint32_t h, int *bad) {   // holds G
    if (h == H_CUR_THREAD) return 0;
    unsigned i = h - 0x1000u;
    if (i == 0 || i >= MAX_H) { *bad = 1; return 0; }
    switch (handles[i].type) {
    case O_THREAD: return ((struct thr *)handles[i].obj)->exited;
    case O_EVENT_R: return ((struct event *)handles[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}
static int64_t now_mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ll + ts.tv_nsec; }

uint32_t wonder_event_signal(uint32_t handle) {   // host threads (CADisplayLink) signal a write end
    pthread_mutex_lock(&G);
    struct event *e = h_peek(handle, O_EVENT_W);
    if (e) { e->signaled = 1; wake_sync_waiters(); }
    pthread_mutex_unlock(&G);
    return e ? 0 : R_BAD_HANDLE;
}

int wonder_event_call(int number, uint64_t *r) {
    struct thr *me = self();
    switch (number) {
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x25:
    case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x34: case 0x35:
    case 0x45: case 0x11: case 0x12: case 0x17: case 0x18: case 0x16: break;
    default: return 0;
    }
    // Every caller of these SVCs must be a registered guest thread; anything else is a host defect.
    if (!me) { report("KERNEL_NON_GUEST_CALLER svc=%#x", number); return 0; }
    switch (number) {
    case 0x08: r[0] = create_thread(r); return 1;
    case 0x09: {
        pthread_mutex_lock(&G);
        struct thr *t = h_peek((uint32_t)r[0], O_THREAD);
        uint32_t result = !t ? R_BAD_HANDLE : t->started ? R_BAD_STATE : 0;
        if (!result) { t->started = 1; pthread_cond_signal(&t->cv); }
        pthread_mutex_unlock(&G);
        r[0] = result; return 1;
    }
    case 0x0A: exit_thread(me); return 1;
    case 0x0B: {
        int64_t ns = (int64_t)r[0];
        if (ns > 0) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; while (nanosleep(&ts, &ts) && errno == EINTR) {} }
        else sched_yield();   // 0 / -1 / -2: the yield variants
        return 1;
    }
    case 0x0C: { pthread_mutex_lock(&G); struct thr *t = h_peek((uint32_t)r[1], O_THREAD);
                 r[0] = t ? 0 : R_BAD_HANDLE; r[1] = t ? (uint64_t)t->prio : 0; pthread_mutex_unlock(&G); return 1; }
    case 0x0D: {
        int prio = (int)r[1];
        pthread_mutex_lock(&G); struct thr *t = h_peek((uint32_t)r[0], O_THREAD);
        uint32_t result = !t ? R_BAD_HANDLE : (prio < 0 || prio > 63) ? R_INVALID_PRIORITY : 0;
        if (!result) t->prio = prio;
        pthread_mutex_unlock(&G);
        if (!result && t == me) apply_priority(prio);   // other threads pick it up when they start
        r[0] = result; return 1;
    }
    case 0x0E: { pthread_mutex_lock(&G); struct thr *t = h_peek((uint32_t)r[2], O_THREAD);
                 r[0] = t ? 0 : R_BAD_HANDLE; r[1] = t ? (uint64_t)(uint32_t)t->core : 0; r[2] = t ? t->mask : 0;
                 pthread_mutex_unlock(&G); return 1; }
    case 0x0F: {   // Atmosphere kern_svc_thread.cpp: -1 = don't care (ideal 0 here), -2 = process default, -3 = keep
        int core = (int)r[1]; uint64_t mask = r[2];
        pthread_mutex_lock(&G); struct thr *t = h_peek((uint32_t)r[0], O_THREAD);
        uint32_t result = 0;
        if (!t) result = R_BAD_HANDLE;
        else if (core == -2) { core = 0; mask = 1; }
        else if (!mask) result = R_INVALID_COMBINATION;
        else if (mask & ~(uint64_t)PROCESS_CORE_MASK) result = R_INVALID_CORE;
        else if (core >= 0 && (core > 2 || !(mask >> core & 1))) result = R_INVALID_COMBINATION;
        else if (core != -1 && core != -3 && core < 0) result = R_INVALID_CORE;
        if (!result) { if (core >= 0) t->core = core; else if (core == -1) t->core = __builtin_ctzll(mask); t->mask = mask; }
        pthread_mutex_unlock(&G);
        r[0] = result; return 1;   // host scheduling stays with iOS; the virtual core is bookkeeping
    }
    case 0x10: r[0] = (uint64_t)(uint32_t)me->core; return 1;
    case 0x25: { pthread_mutex_lock(&G); struct thr *t = h_peek((uint32_t)r[1], O_THREAD);
                 r[0] = t ? 0 : R_BAD_HANDLE; r[1] = t ? t->id : 0; pthread_mutex_unlock(&G); return 1; }
    case 0x1A: {
        uint64_t addr = r[1]; int64_t result = 0;
        if (!addr || (addr & 3)) { r[0] = R_INVALID_POINTER; return 1; }
        pthread_mutex_lock(&G);
        if (ld32(addr) == ((uint32_t)r[0] | WAITERS_BIT)) { me->waddr = addr; me->wtag = (uint32_t)r[2]; result = block(me, W_LOCK, -1); }
        pthread_mutex_unlock(&G);
        r[0] = (uint64_t)result; return 1;
    }
    case 0x1B:
        if (!r[0] || (r[0] & 3)) { r[0] = R_INVALID_POINTER; return 1; }
        pthread_mutex_lock(&G); lock_handoff(r[0]); pthread_mutex_unlock(&G); r[0] = 0; return 1;
    case 0x1C: {
        uint64_t addr = r[0], key = r[1];
        if (!addr || (addr & 3)) { r[0] = R_INVALID_POINTER; return 1; }
        pthread_mutex_lock(&G);
        // Set the condition key before releasing the mutex: user-mode Signal skips svc 0x1D when it reads 0.
        st32(key, 1);
        me->wkey = key; me->waddr = addr; me->wtag = (uint32_t)r[2];
        lock_handoff(addr);
        int64_t result = block(me, W_CONDVAR, (int64_t)r[3]);   // on timeout user mode re-acquires the lock itself
        pthread_mutex_unlock(&G);
        r[0] = (uint64_t)result; return 1;
    }
    case 0x1D: {
        uint64_t key = r[0]; int count = (int)r[1], n = 0;
        pthread_mutex_lock(&G);
        for (struct thr *w; (count <= 0 || n < count) && (w = pick(W_CONDVAR, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);
        int left = 0; for (struct thr *w = waiting; w; w = w->next) if (w->wkind == W_CONDVAR && w->wkey == key) { left = 1; break; }
        if (!left) st32(key, 0);
        pthread_mutex_unlock(&G);
        r[0] = 0; return 1;
    }
    case 0x34: {
        uint64_t addr = r[0]; int type = (int)r[1]; int32_t value = (int32_t)r[2]; int64_t result;
        if (!addr || (addr & 3)) { r[0] = R_INVALID_POINTER; return 1; }
        if (type < 0 || type > 2) { r[0] = 0xF001u; return 1; }   // invalid enum
        pthread_mutex_lock(&G);
        uint32_t v = ld32(addr);
        int ok = type == 2 ? (int32_t)v == value : (int32_t)v < value;
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < value)) { ok = 0; break; } }
        if (!ok) result = R_BAD_STATE;
        else { me->waddr = addr; result = block(me, W_ADDRESS, (int64_t)r[3]); }
        pthread_mutex_unlock(&G);
        r[0] = (uint64_t)result; return 1;
    }
    case 0x35: {
        uint64_t addr = r[0]; int type = (int)r[1]; int32_t value = (int32_t)r[2]; int count = (int)r[3];
        if (!addr || (addr & 3)) { r[0] = R_INVALID_POINTER; return 1; }
        if (type < 0 || type > 2) { r[0] = 0xF001u; return 1; }
        pthread_mutex_lock(&G);
        int nw = 0; for (struct thr *w = waiting; w; w = w->next) if (w->wkind == W_ADDRESS && w->waddr == addr) nw++;
        int64_t result = 0;
        if (type != 0) {
            int32_t nv = value + 1;
            if (type == 2) {
                if (count <= 0) nv = nw > 0 ? value - 1 : value + 1;
                else nv = nw == 0 ? value + 1 : nw <= count ? value - 1 : value;
            }
            uint32_t e = (uint32_t)value;
            if (!cas32(addr, &e, (uint32_t)nv)) result = R_BAD_STATE;
        }
        if (!result) { int n = 0; for (struct thr *w; (count <= 0 || n < count) && (w = pick(W_ADDRESS, addr)); n++) wake(w, 0); }
        pthread_mutex_unlock(&G);
        r[0] = (uint64_t)result; return 1;
    }
    case 0x45: {
        struct event *e = calloc(1, sizeof *e);
        if (!e) { r[0] = R_OUT_OF_RESOURCE; return 1; }
        e->refs = 2;
        pthread_mutex_lock(&G);
        uint32_t hw = h_new(O_EVENT_W, e), hr = hw ? h_new(O_EVENT_R, e) : 0;
        if (!hr) { if (hw) handles[hw - 0x1000u].type = O_FREE; free(e); }
        pthread_mutex_unlock(&G);
        if (!hr) { r[0] = R_OUT_OF_RESOURCE; return 1; }
        r[0] = 0; r[1] = hw; r[2] = hr; return 1;
    }
    case 0x11: case 0x12: case 0x17: {   // Signal: write end; Clear: either end; ResetSignal: read end
        uint32_t h = (uint32_t)r[0];
        pthread_mutex_lock(&G);
        struct event *e = number == 0x11 ? h_peek(h, O_EVENT_W) : number == 0x17 ? h_peek(h, O_EVENT_R)
                        : (h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R));
        uint32_t result = 0;
        if (!e) result = R_BAD_HANDLE;
        else if (number == 0x11) { e->signaled = 1; wake_sync_waiters(); }
        else if (number == 0x17 && !e->signaled) result = R_BAD_STATE;
        else e->signaled = 0;
        pthread_mutex_unlock(&G);
        r[0] = result; return 1;
    }
    case 0x18: {
        const uint32_t *hs = (const uint32_t *)r[1]; int n = (int32_t)r[2]; int64_t ns = (int64_t)r[3];
        if (n < 0 || n > 64) { r[0] = R_OUT_OF_RANGE; return 1; }
        if (n && !hs) { r[0] = R_INVALID_POINTER; return 1; }
        int64_t deadline = ns > 0 ? now_mono_ns() + ns : 0, result; int index = -1;
        pthread_mutex_lock(&G);
        for (;;) {
            if (me->cancel) { me->cancel = 0; result = R_CANCELLED; break; }
            int bad = 0;
            for (int i = 0; i < n && index < 0; i++) if (signaled(hs[i], &bad)) index = i;
            if (bad) { result = R_BAD_HANDLE; break; }
            if (index >= 0) { result = 0; break; }
            int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : deadline - now_mono_ns();
            if (ns > 0 && left <= 0) { result = R_TIMEOUT; break; }
            if (block(me, W_SYNC, left) == R_TIMEOUT) { result = R_TIMEOUT; break; }
        }
        pthread_mutex_unlock(&G);
        r[0] = (uint64_t)result; r[1] = index >= 0 ? (uint64_t)index : (uint64_t)-1; return 1;
    }
    case 0x19: {
        pthread_mutex_lock(&G);
        struct thr *t = h_peek((uint32_t)r[0], O_THREAD);
        if (t) { t->cancel = 1; if (t->wkind == W_SYNC) wake(t, -1); }
        pthread_mutex_unlock(&G);
        r[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x16: {   // events free after both ends close; thread objects after close + exit
        uint32_t h = (uint32_t)r[0]; unsigned i = h - 0x1000u; uint32_t result = 0;
        pthread_mutex_lock(&G);
        if (!owned(h) || h == H_CUR_THREAD) result = R_BAD_HANDLE;
        else if (handles[i].type == O_EVENT_R || handles[i].type == O_EVENT_W) {
            struct event *e = handles[i].obj; if (--e->refs == 0) free(e);
            handles[i].type = O_FREE; handles[i].obj = NULL;
        } else if (handles[i].type == O_THREAD) {
            struct thr *t = handles[i].obj; handles[i].type = O_FREE; handles[i].obj = NULL;
            t->closed = 1; if (t->exited) free_thread(t); else if (!t->started) pthread_cond_signal(&t->cv);
        }
        pthread_mutex_unlock(&G);
        r[0] = result; return 1;
    }
    }
    return 0;
}
