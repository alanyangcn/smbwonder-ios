#include "dm.h"
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
static int capability,probe_result,driver_error;
struct dm_flags g_flag;
int wonder_host_graphics_probe(void){return capability;}
int wonder_metal3_selftest(void){return probe_result;}
int wonder_host_driver_start(void *m,unsigned long n){assert(m&&n);return driver_error;}
_Noreturn void wonder_game_thread_stop(const char *reason){
    if(capability<0)assert(strstr(reason,"GRAPHICS_DEVICE_UNAVAILABLE"));
    else if(!capability)assert(strstr(reason,"GRAPHICS_DEVICE_UNSUPPORTED"));
    else if(probe_result)assert(strstr(reason,"METAL3_PROBE_FAILED"));
    else {assert(driver_error);assert(strstr(reason,"HOST_GRAPHICS_DRIVER_FAILED"));}
    _exit(42);
}
#include "../runtime/graphics_gate.c"
int main(void){
    for(int c=-1;c<=1;c++)for(int error=0;error<=1;error++)for(int driver=0;driver<=1;driver++){
        pid_t p=fork();assert(p>=0);if(!p){capability=c;probe_result=error;driver_error=driver?19:0;h_graphics_init((void*)0x10000,0x1000000);_exit(99);}
        int status;assert(waitpid(p,&status,0)==p);assert(WIFEXITED(status));
        // Only a Metal 3 GPU with a passing probe and an established host driver may return to the guest.
        int returns=c==1&&!error&&!driver;
        assert(WEXITSTATUS(status)==(returns?99:42));
    }
    puts("GRAPHICS_BOUNDARY_RETURNS_ONLY_WITH_HOST_DRIVER_OK");
}
