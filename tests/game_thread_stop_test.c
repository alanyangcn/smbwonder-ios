#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern int wonder_game_thread_start(pthread_t *,void *(*)(void *),void *);
extern void wonder_game_thread_set_reporter(void (*)(const char *));
extern void wonder_game_thread_stop(const char *);
static int reported,continued;
static void report(const char *message){assert(!strcmp(message,"missing service"));reported++;}
static void *entry(void *context){(void)context;wonder_game_thread_stop("missing service");continued++;return NULL;}
int main(void){pthread_t thread;wonder_game_thread_set_reporter(report);assert(!wonder_game_thread_start(&thread,entry,NULL));void *status=NULL;assert(!pthread_join(thread,&status));assert((uintptr_t)status==ENOSYS);assert(reported==1&&continued==0);puts("GAME_THREAD_TERMINAL_STOP_OK");}
