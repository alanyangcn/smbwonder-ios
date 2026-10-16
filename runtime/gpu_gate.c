#include "gpu_gate.h"
#include <pthread.h>
#include <time.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t opened = PTHREAD_COND_INITIALIZER;
static int open_ = 1;
static uint64_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
void wonder_gpu_gate_set(int open) {
    pthread_mutex_lock(&lock); open_ = open != 0; if (open_) pthread_cond_broadcast(&opened); pthread_mutex_unlock(&lock);
}
int wonder_gpu_gate_is_open(void) { pthread_mutex_lock(&lock); int o = open_; pthread_mutex_unlock(&lock); return o; }
uint64_t wonder_gpu_gate_wait(void) {
    pthread_mutex_lock(&lock);
    if (open_) { pthread_mutex_unlock(&lock); return 0; }
    uint64_t start = now();
    while (!open_) pthread_cond_wait(&opened, &lock);
    pthread_mutex_unlock(&lock);
    uint64_t waited = now() - start; return waited ? waited : 1;
}
