#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
static _Atomic int released;
static void *original;
static void tracked_free(void *p) { assert(p == original); released++; free(p); }
#define free tracked_free
#include "guest_tls.h"
#undef free
static pthread_key_t tls_key, tlr_key;
static void *worker(void *unused) {
    (void)unused;
    assert(!wonder_guest_tls_prepare(tls_key, tlr_key));
    uint8_t *tlr = pthread_getspecific(tlr_key), *tls = pthread_getspecific(tls_key);
    original = tlr;
    assert(*(void **)(tlr + 0x1f0) == tls && tls == tlr + 0x200);
    assert(wonder_guest_tls_prepare(tls_key, tlr_key) != 0);
    // SDK changes TPIDR_EL0/TLR keys; the ownership key must keep the original allocation.
    pthread_setspecific(tls_key, (void *)0x1234); pthread_setspecific(tlr_key, (void *)0x5678);
    return NULL;
}
int main(void) {
    assert(!pthread_key_create(&tls_key, NULL) && !pthread_key_create(&tlr_key, NULL));
    pthread_t t; assert(!pthread_create(&t, NULL, worker, NULL)); assert(!pthread_join(t, NULL));
    assert(released == 1);
}
