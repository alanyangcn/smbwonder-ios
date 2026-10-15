// Exercise a real guest-stack switch on the host. Exit must publish completion on the host stack.
#define _XOPEN_SOURCE 600
#include "kernel_events.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <unistd.h>
static void *guest_stack;
static const size_t guest_size = 128 * 1024;
static pthread_key_t cleanup_key;
static _Atomic int retired;
static void cleanup(void *unused) {
    (void)unused; char local;
    uintptr_t sp = (uintptr_t)&local, lo = (uintptr_t)guest_stack;
    assert((sp < lo || sp >= lo + guest_size) && "ExitThread still uses recyclable guest stack");
    retired = 1;
}
static void prepare(void) { assert(!pthread_setspecific(cleanup_key, (void *)1)); }
static void guest(void) { uint64_t r[8] = {0}; assert(wonder_event_call(0x0a, r)); abort(); }
static void enter(uint64_t entry, uint64_t arg, uint64_t sp) {
    (void)entry; (void)arg; (void)sp;
    ucontext_t host, alternate;
    assert(!getcontext(&alternate));
    alternate.uc_stack.ss_sp = guest_stack; alternate.uc_stack.ss_size = guest_size;
    alternate.uc_link = NULL;
    makecontext(&alternate, guest, 0);
    assert(!swapcontext(&host, &alternate)); abort();
}
int main(void) {
    guest_stack = malloc(guest_size); assert(guest_stack);
    assert(!pthread_key_create(&cleanup_key, cleanup));
    assert(wonder_threads_register_current(44, 0, prepare, enter, NULL));
    uint64_t r[8] = {0, 1, 0, ((uintptr_t)guest_stack + guest_size) & ~(uintptr_t)15, 44, 0};
    assert(wonder_event_call(8, r) && !r[0]); uint32_t h = r[1];
    r[0] = h; assert(wonder_event_call(9, r) && !r[0]);
    uint64_t wait[8] = {0, (uintptr_t)&h, 1, 2000000000};
    assert(wonder_event_call(0x18, wait) && !wait[0]);
    for (int i = 0; i < 2000 && !retired; i++) usleep(1000);
    assert(retired);
    free(guest_stack); puts("KERNEL_THREAD_EXIT_HOST_STACK_OK");
}
