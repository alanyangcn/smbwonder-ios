// A dedicated, joinable host thread with the exact target NPDM main stack budget.
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int started;
int wonder_game_thread_start(pthread_t *thread,void *(*entry)(void *),void *context) {
    if(!thread||!entry)return EINVAL;
    pthread_mutex_lock(&lock);
    if(started){pthread_mutex_unlock(&lock);return EALREADY;}
    pthread_attr_t attributes;int error=pthread_attr_init(&attributes);
    if(error){pthread_mutex_unlock(&lock);return error;}
    size_t guard=(size_t)getpagesize(),total=guard+0x100000;
    void *mapping=mmap(NULL,total,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(mapping==MAP_FAILED){pthread_attr_destroy(&attributes);pthread_mutex_unlock(&lock);return ENOMEM;}
    void *stack=(char *)mapping+guard;
    error=mprotect(stack,0x100000,PROT_READ|PROT_WRITE)?errno:0;
    // iOS may enlarge an automatically allocated pthread stack with metadata/alignment pages.
    // Supply the stack explicitly so the guest sees precisely its NPDM stack, with a separate guard.
    if(!error)error=pthread_attr_setstack(&attributes,stack,0x100000);
    if(!error)error=pthread_create(thread,&attributes,entry,context);
    pthread_attr_destroy(&attributes);
    if(!error)started=1;
    else munmap(mapping,total);
    pthread_mutex_unlock(&lock);return error;
}

// A missing service ends the guest thread. It never returns success to the guest.
// Keep the UIKit host alive so a diagnostic can display the exact blocking interface.
#include <stdint.h>
// With several guest threads only the first stop is the boundary; later stops (other threads reaching a
// gap, or SVCs after the stop) are logged as secondary and those threads are parked, never resumed.
#include <stdatomic.h>
#include <stdio.h>
static void (*stop_reporter)(const char *);
static atomic_int stopped;
void wonder_game_thread_set_reporter(void (*reporter)(const char *)) { stop_reporter=reporter; }
int wonder_game_stopped(void) { return atomic_load(&stopped); }
void wonder_game_report_stop(const char *reason) {
    int expected=0;
    if(atomic_compare_exchange_strong(&stopped,&expected,1)) { if(stop_reporter)stop_reporter(reason); }
    else fprintf(stderr,"GUEST_STOP_SECONDARY %s\n",reason);
}
_Noreturn void wonder_game_thread_stop(const char *reason) {
    wonder_game_report_stop(reason);
    pthread_exit((void *)(uintptr_t)ENOSYS);
}
_Noreturn void wonder_game_thread_park(void) {
    static pthread_mutex_t park=PTHREAD_MUTEX_INITIALIZER;static pthread_cond_t never=PTHREAD_COND_INITIALIZER;
    pthread_mutex_lock(&park);for(;;)pthread_cond_wait(&never,&park);
}
