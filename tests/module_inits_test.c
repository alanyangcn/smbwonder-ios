#include "dm.h"
#include <assert.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
extern void wonder_module_inits(module_t *,int);
static int calls;
static void first(void){assert(calls++==0);}
static void second(void){assert(calls++==1);}
int main(void) {
    uintptr_t base=((uintptr_t)first<(uintptr_t)second?(uintptr_t)first:(uintptr_t)second)&~0xfffull;
    module_t modules[2]={{.base=(uint8_t*)base,.size=0x4000,.text_end=0x4000,.init=(uintptr_t)first-base},
                        {.base=(uint8_t*)base,.size=0x4000,.text_end=0x4000,.init=(uintptr_t)second-base}};
    strcpy(modules[0].name,"main");strcpy(modules[1].name,"sdk");
    wonder_module_inits(modules,2);assert(calls==2);
    pid_t child=fork();assert(child>=0);
    if(!child){modules[0].init=modules[0].size+4;wonder_module_inits(modules,2);_exit(0);}
    int status;assert(waitpid(child,&status,0)==child);assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGABRT);
    puts("MODULE_INITIALIZERS_TEST_OK");
}
