#include "gpu_gate.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
static volatile int passed;
static void *submitter(void *arg) { (void)arg; uint64_t waited = wonder_gpu_gate_wait(); passed = waited > 0 ? 2 : 1; return NULL; }
int main(void) {
    assert(wonder_gpu_gate_is_open() && wonder_gpu_gate_wait() == 0);   // starts open: no wait
    wonder_gpu_gate_set(0); assert(!wonder_gpu_gate_is_open());
    pthread_t t; pthread_create(&t, NULL, submitter, NULL);
    usleep(50000); assert(passed == 0);                                     // blocked while the app is not active
    wonder_gpu_gate_set(1); pthread_join(t, NULL); assert(passed == 2);   // released, reports a wait
    assert(wonder_gpu_gate_wait() == 0);
    puts("gpu_gate OK"); return 0;
}
