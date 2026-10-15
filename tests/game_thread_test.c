#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
extern int wonder_game_thread_start(pthread_t *,void *(*)(void *),void *);
static pthread_t caller;
static void *check(void *context) {
    assert(!pthread_equal(caller,pthread_self()));
    assert(pthread_get_stacksize_np(pthread_self())==0x100000);
    uintptr_t top=(uintptr_t)pthread_get_stackaddr_np(pthread_self());
    volatile int local=0;
    assert((uintptr_t)&local<top && (uintptr_t)&local>=top-0x100000);
    assert((uintptr_t)context==0x1234);
    return (void *)(uintptr_t)0x5678;
}
int main(void) {
    caller=pthread_self();pthread_t thread;
    assert(!wonder_game_thread_start(&thread,check,(void *)(uintptr_t)0x1234));
    void *result=NULL;assert(!pthread_join(thread,&result));assert((uintptr_t)result==0x5678);
    assert(wonder_game_thread_start(&thread,check,NULL)==EALREADY);
    puts("GAME_MAIN_THREAD_TEST_OK");
}
