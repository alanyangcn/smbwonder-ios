#pragma once
// One allocation owned by a separate key: guest writes to TLS/TLR keys cannot lose its ownership.
// Included only by sdk_startup.c (and the host ownership regression test).
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
static pthread_key_t wonder_tls_owner;
static pthread_once_t wonder_tls_owner_once = PTHREAD_ONCE_INIT;
static int wonder_tls_owner_error;
static void wonder_tls_owner_destroy(void *allocation) { free(allocation); }
static void wonder_tls_owner_create(void) {
    wonder_tls_owner_error = pthread_key_create(&wonder_tls_owner, wonder_tls_owner_destroy);
}
static int wonder_guest_tls_prepare(pthread_key_t tls_key, pthread_key_t tlr_key) {
    int error = pthread_once(&wonder_tls_owner_once, wonder_tls_owner_create);
    if (error || wonder_tls_owner_error) return error ? error : wonder_tls_owner_error;
    if (pthread_getspecific(wonder_tls_owner)) return EALREADY;
    uint8_t *tlr = calloc(1, 0x2200);
    if (!tlr) return ENOMEM;
    uint8_t *tls = tlr + 0x200;
    *(void **)(tlr + 0x1f0) = tls;
    error = pthread_setspecific(wonder_tls_owner, tlr);
    if (!error) error = pthread_setspecific(tls_key, tls);
    if (!error) error = pthread_setspecific(tlr_key, tlr);
    if (error) {
        pthread_setspecific(wonder_tls_owner, NULL);
        pthread_setspecific(tls_key, NULL); pthread_setspecific(tlr_key, NULL);
        free(tlr);
    }
    return error;
}
