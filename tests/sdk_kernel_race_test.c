// Pause the final host-page reset while another SVC attempts to map its neighbour.
#include "dm.h"
#include <sys/mman.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static void *gated_address;
static int resetting, release_reset, map_started, map_done;
static void *gated_mmap(void *address, size_t size, int prot, int flags, int fd, off_t offset) {
    if (address && address == gated_address && size == 0x4000 && prot == PROT_NONE) {
        pthread_mutex_lock(&gate); resetting = 1; pthread_cond_broadcast(&changed);
        while (!release_reset) pthread_cond_wait(&changed, &gate);
        pthread_mutex_unlock(&gate);
    }
    return mmap(address, size, prot, flags, fd, offset);
}
#define mmap gated_mmap
#include "../runtime/sdk_kernel.c"
#undef mmap
struct dm_flags g_flag;
static void *unmap_page(void *p) {
    uint64_t r[31] = {(uint64_t)p, 0x1000}; assert(wonder_kernel_call(0x2d, r) && !r[0]); return NULL;
}
static void *map_page(void *p) {
    pthread_mutex_lock(&gate); map_started = 1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&gate);
    uint64_t r[31] = {(uint64_t)p, 0x1000}; assert(wonder_kernel_call(0x2c, r) && !r[0]);
    *(volatile int *)p = 42;
    pthread_mutex_lock(&gate); map_done = 1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&gate); return NULL;
}
int main(void) {
    uint64_t r[31] = {0}; wonder_kernel_init(0x5000000);
    r[1] = 2; assert(wonder_kernel_call(0x29, r) && !r[0]); uint64_t address = r[1];
    r[0] = address; r[1] = 0x1000; assert(wonder_kernel_call(0x2c, r) && !r[0]);
    gated_address = (void *)address; pthread_t first, second;
    assert(!pthread_create(&first, NULL, unmap_page, (void *)address));
    pthread_mutex_lock(&gate); while (!resetting) pthread_cond_wait(&changed, &gate); pthread_mutex_unlock(&gate);
    assert(!pthread_create(&second, NULL, map_page, (void *)(address + 0x1000)));
    pthread_mutex_lock(&gate); while (!map_started) pthread_cond_wait(&changed, &gate);
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec++;
    while (!map_done && !pthread_cond_timedwait(&changed, &gate, &deadline)) {}
    int raced = map_done; release_reset = 1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&gate);
    pthread_join(first, NULL); pthread_join(second, NULL);
    assert(!raced && "map completed while the same host page was being reset");
    assert(*(volatile int *)(address + 0x1000) == 42 && alias_host[0] == 1);
    puts("SDK_KERNEL_CONCURRENT_MAPPING_OK");
}
