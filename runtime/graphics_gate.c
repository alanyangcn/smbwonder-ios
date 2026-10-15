// This boundary replaces the console driver only when a real host Metal context exists.
// Never run nvdrv IPC on iOS. Returning means only that the host Metal context exists, not that a renderer works.
#include "hle.h"
extern int wonder_host_graphics_probe(void);
extern const char *wonder_graphics_backend(void);
extern int wonder_metal2_selftest(void);
extern int wonder_host_driver_start(void *,unsigned long);
_Noreturn void wonder_game_thread_stop(const char *);
HLE_FORCE("_ZN2nv18InitializeGraphicsEPvm","Validate actual GPU and establish persistent host Metal context; NVN itself is host-implemented",void,h_graphics_init,void *memory,unsigned long size){
    LOG("GRAPHICS_BOUNDARY nv::InitializeGraphics memory=%p size=%#lx",memory,size);
    int capability=wonder_host_graphics_probe();
    if(capability<0)wonder_game_thread_stop("GRAPHICS_DEVICE_UNAVAILABLE: no Metal device; capability not established");
    const char *backend=wonder_graphics_backend();
    LOG("GRAPHICS_SELECTED backend=%s",backend);
    if(!strcmp(backend,"metal2")) {
        if(wonder_metal2_selftest())wonder_game_thread_stop("METAL2_PROBE_FAILED: shader, binding or GPU readback validation failed; inspect metal2-probe.json");
        // Return only once a persistent host Metal device, queue and CAMetalLayer exist.
        // This is the console driver boundary only; NVN objects are created later by nvn_device.c.
        int error=wonder_host_driver_start(memory,size);
        if(error){char reason[128];snprintf(reason,sizeof reason,"HOST_GRAPHICS_DRIVER_FAILED error=%d: persistent Metal device/queue/layer not established",error);wonder_game_thread_stop(reason);}
        LOG("GRAPHICS_DRIVER_READY backend=metal2 host Metal device/queue/CAMetalLayer persistent; NVN device not created yet");
        return;
    }
    if(strcmp(backend,"metal3"))wonder_game_thread_stop("GRAPHICS_BACKEND_INVALID: select metal2 or metal3");
    if(!capability)wonder_game_thread_stop("DEVICE_UPGRADE_REQUIRED: current GPU lacks Metal 3 required by the selected D2R NVN-to-Metal path; use a newer device for graphics work");
    wonder_game_thread_stop("GRAPHICS_BACKEND_PENDING: Metal 3 capability passed; Wonder NVN integration, target shader translation and first-frame validation still required");
}
