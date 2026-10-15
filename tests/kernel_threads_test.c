// Host test of kernel_threads.c: guest threads, events, wait/cancel, lock/condvar and address arbitration.
#include "kernel_events.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static uint64_t svc(int n, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t *out1) {
    uint64_t r[8] = {a, b, c, d, e, f, 0, 0}; assert(wonder_event_call(n, r)); if (out1) *out1 = r[1]; return r[0];
}
static _Atomic int prepared, retired;
static pthread_key_t retirement_key;
static pthread_once_t retirement_once = PTHREAD_ONCE_INIT;
static void retire(void *p) { (void)p; retired++; }
static void make_retirement_key(void) { assert(!pthread_key_create(&retirement_key, retire)); }
static void prepare(void) { pthread_once(&retirement_once, make_retirement_key); pthread_setspecific(retirement_key, (void *)1); prepared++; }
// Test trampoline: run the "guest" entry as a host function on the pthread stack, then ExitThread.
static void enter(uint64_t entry, uint64_t arg, uint64_t sp) { (void)sp; ((void (*)(uint64_t))entry)(arg); uint64_t r[8] = {0}; wonder_event_call(0x0A, r); }
static void sink(const char *line) { printf("%s\n", line); }
static _Alignas(16) unsigned char fake_stack[4096];
static uint64_t sp_top(void) { return (uint64_t)(fake_stack + sizeof fake_stack); }

static uint32_t read_end, write_end; static _Atomic int phase;
static void waiter(uint64_t arg) {   // waits on the event, then signals completion through the address arbiter
    (void)arg; uint64_t index;
    atomic_store(&phase, 1);
    assert(svc(0x18, 0, (uint64_t)&read_end, 1, (uint64_t)-1, 0, 0, &index) == 0 && index == 0);
    atomic_store(&phase, 2);
}
static uint32_t lock_word, cv_key; static int shared;
static uint64_t my_id(void) { uint64_t id; assert(svc(0x25, 0, 0xFFFF8000u, 0, 0, 0, 0, &id) == 0); return id; }
static void lock(uint32_t tag) {   // user-mode half of nn::os mutex (simplified, as the SDK does)
    for (;;) {
        uint32_t expect = 0;
        if (__atomic_compare_exchange_n(&lock_word, &expect, tag, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return;
        uint32_t owner = expect & ~0x40000000u;
        if (__atomic_compare_exchange_n(&lock_word, &expect, expect | 0x40000000u, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) || (expect & 0x40000000u)) {
            assert(svc(0x1A, owner, (uint64_t)&lock_word, tag, 0, 0, 0, 0) == 0);
            if ((__atomic_load_n(&lock_word, __ATOMIC_SEQ_CST) & ~0x40000000u) == tag) return;
        }
    }
}
static void unlock(uint32_t tag) {
    uint32_t expect = tag;
    if (!__atomic_compare_exchange_n(&lock_word, &expect, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) assert(svc(0x1B, (uint64_t)&lock_word, 0, 0, 0, 0, 0, 0) == 0);
}
static void consumer(uint64_t arg) {
    uint32_t me = (uint32_t)arg;   // lock tag (the SDK uses the owner's thread handle)
    lock(me);
    while (!shared) {   // condvar wait releases the lock in the kernel and re-acquires it on wake
        assert(svc(0x1C, (uint64_t)&lock_word, (uint64_t)&cv_key, me, (uint64_t)-1, 0, 0, 0) == 0);
        assert((lock_word & ~0x40000000u) == me);
    }
    shared = 2; unlock(me);
}
static int32_t counter;
static void address_waiter(uint64_t arg) {
    (void)arg;
    assert(svc(0x34, (uint64_t)&counter, 2, 0, (uint64_t)-1, 0, 0, 0) == 0);   // wait while counter == 0
}
int main(void) {
    uint64_t r[8] = {0};
    assert(!wonder_event_call(0x18, r));                          // non-guest caller is never served
    uint32_t main_handle = wonder_threads_register_current(44, 0, prepare, enter, sink);
    assert(main_handle == 0x1001 && my_id() == 0x51);
    assert(!wonder_event_call(0x7f, r));
    // Events: create, wait with timeout, cross-thread wake, ResetSignal semantics, close lifetime.
    uint64_t hr;
    assert(svc(0x45, 0, 0, 0, 0, 0, 0, &hr) == 0);
    { uint64_t q[8] = {0}; assert(wonder_event_call(0x45, q) && !q[0]); write_end = (uint32_t)q[1]; read_end = (uint32_t)q[2]; }
    assert(svc(0x18, 0, (uint64_t)&read_end, 1, 0, 0, 0, 0) == 0xEA01);
    assert(svc(0x18, 0, (uint64_t)&read_end, 1, 1000000, 0, 0, 0) == 0xEA01);
    uint64_t th;
    assert(svc(0x08, 0, (uint64_t)waiter, 0, sp_top() + 8, 44, 0, 0) == 0xCC01);   // misaligned stack
    assert(svc(0x08, 0, (uint64_t)waiter, 0, sp_top(), 64, 0, 0) == 0xE001);       // bad priority
    assert(svc(0x08, 0, (uint64_t)waiter, 0, sp_top(), 44, 3, 0) == 0xE201);       // core outside mask 0-2
    assert(svc(0x08, 0, (uint64_t)waiter, 0, sp_top(), 44, 1, &th) == 0);
    usleep(20000); assert(atomic_load(&phase) == 0);               // created but not started
    assert(svc(0x09, th, 0, 0, 0, 0, 0, 0) == 0 && svc(0x09, th, 0, 0, 0, 0, 0, 0) == 0xFA01);
    while (atomic_load(&phase) != 1) usleep(1000);
    usleep(20000); assert(atomic_load(&phase) == 1);
    assert(wonder_event_signal(read_end) == 0xE401 && wonder_event_signal(write_end) == 0);
    uint32_t thread_handle = (uint32_t)th; uint64_t index;
    assert(svc(0x18, 0, (uint64_t)&thread_handle, 1, (uint64_t)-1, 0, 0, &index) == 0 && index == 0);   // thread exit signals its handle
    assert(atomic_load(&phase) == 2 && prepared == 1);
    assert(svc(0x18, 0, (uint64_t)&read_end, 1, 0, 0, 0, 0) == 0);   // waiting never consumes the signal
    assert(svc(0x17, read_end, 0, 0, 0, 0, 0, 0) == 0 && svc(0x17, read_end, 0, 0, 0, 0, 0, 0) == 0xFA01);
    assert(svc(0x11, read_end, 0, 0, 0, 0, 0, 0) == 0xE401);
    assert(svc(0x16, th, 0, 0, 0, 0, 0, 0) == 0 && svc(0x16, th, 0, 0, 0, 0, 0, 0) == 0xE401);
    // Cancel: a pending wait is interrupted.
    // (CancelSynchronization targets another thread; here cancel ourselves before waiting.)
    assert(svc(0x19, 0x1001, 0, 0, 0, 0, 0, 0) == 0);
    assert(svc(0x18, 0, (uint64_t)&read_end, 1, (uint64_t)-1, 0, 0, 0) == 0xEC01);
    // Lock + condition variable across two guest threads.
    assert(svc(0x08, 0, (uint64_t)consumer, 0x77, sp_top(), 40, 2, &th) == 0 && svc(0x09, th, 0, 0, 0, 0, 0, 0) == 0);
    while (!__atomic_load_n(&cv_key, __ATOMIC_SEQ_CST)) usleep(1000);   // consumer is waiting
    lock(0x1001); shared = 1;
    assert(svc(0x1D, (uint64_t)&cv_key, 1, 0, 0, 0, 0, 0) == 0);
    unlock(0x1001);
    thread_handle = (uint32_t)th;
    assert(svc(0x18, 0, (uint64_t)&thread_handle, 1, 2000000000, 0, 0, 0) == 0 && shared == 2 && lock_word == 0);
    // Address arbiter: SignalToAddress type 1 increments then wakes.
    assert(svc(0x34, (uint64_t)&counter, 2, 1, 0, 0, 0, 0) == 0xFA01);   // value differs: no wait
    assert(svc(0x08, 0, (uint64_t)address_waiter, 0, sp_top(), 44, 0, &th) == 0 && svc(0x09, th, 0, 0, 0, 0, 0, 0) == 0);
    usleep(30000);
    assert(svc(0x35, (uint64_t)&counter, 1, 1, 1, 0, 0, 0) == 0xFA01);   // expected value mismatch
    assert(svc(0x35, (uint64_t)&counter, 1, 0, 1, 0, 0, 0) == 0 && counter == 1);
    thread_handle = (uint32_t)th;
    assert(svc(0x18, 0, (uint64_t)&thread_handle, 1, 2000000000, 0, 0, 0) == 0);
    // Priorities / core masks / ids.
    uint64_t v;
    assert(svc(0x0D, 0x1001, 70, 0, 0, 0, 0, 0) == 0xE001 && svc(0x0D, 0x1001, 30, 0, 0, 0, 0, 0) == 0);
    assert(svc(0x0C, 0, 0x1001, 0, 0, 0, 0, &v) == 0 && v == 30);
    assert(svc(0x0F, 0x1001, 1, 0x8, 0, 0, 0, 0) == 0xE201 && svc(0x0F, 0x1001, 2, 0x1, 0, 0, 0, 0) == 0xE801);
    assert(svc(0x0F, 0x1001, -1, 0x6, 0, 0, 0, 0) == 0 && svc(0x10, 0, 0, 0, 0, 0, 0, 0) == 1);
    assert(svc(0x25, 0, 0x7777, 0, 0, 0, 0, 0) == 0xE401);
    for (int i = 0; i < 2000 && retired < 3; i++) usleep(1000);
    assert(retired == 3);
    int before = prepared;
    assert(svc(0x08, 0, (uint64_t)waiter, 0, sp_top(), 44, 0, &th) == 0);
    for (int i = 0; i < 2000 && prepared == before; i++) usleep(1000);
    assert(prepared == before + 1);
    assert(svc(0x16, th, 0, 0, 0, 0, 0, 0) == 0); // close before StartThread
    for (int i = 0; i < 2000 && retired < 4; i++) usleep(1000);
    assert(retired == 4); // host pthread/TLS must retire without executing the guest
    puts("KERNEL_THREADS_SYNC_OK");
}
