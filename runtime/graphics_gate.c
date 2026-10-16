// This boundary replaces the console driver only when a real host Metal context exists.
// Never run nvdrv IPC on iOS. Returning means only that the host Metal context exists, not that a renderer works.
// The renderer targets Metal 3 (MSL 3.2, native BC sampling, border colours): a GPU without the Metal 3 family stops here.
#include "hle.h"
extern int wonder_host_graphics_probe(void);
extern int wonder_metal3_selftest(void);
extern int wonder_host_driver_start(void *,unsigned long);
_Noreturn void wonder_game_thread_stop(const char *);
HLE_FORCE("_ZN2nv18InitializeGraphicsEPvm","Validate actual GPU and establish persistent host Metal context; NVN itself is host-implemented",void,h_graphics_init,void *memory,unsigned long size){
    LOG("GRAPHICS_BOUNDARY nv::InitializeGraphics memory=%p size=%#lx",memory,size);
    int capability=wonder_host_graphics_probe();
    if(capability<0)wonder_game_thread_stop("GRAPHICS_DEVICE_UNAVAILABLE: no Metal device; capability not established");
    if(!capability)wonder_game_thread_stop("GRAPHICS_DEVICE_UNSUPPORTED: the GPU lacks the Metal 3 family (or BC texture sampling) this renderer requires");
    if(wonder_metal3_selftest())wonder_game_thread_stop("METAL3_PROBE_FAILED: shader, binding or GPU readback validation failed; inspect metal3-probe.json");
    // Return only once a persistent host Metal device, queue and CAMetalLayer exist.
    // This is the console driver boundary only; NVN objects are created later by nvn_device.c.
    int error=wonder_host_driver_start(memory,size);
    if(error){char reason[128];snprintf(reason,sizeof reason,"HOST_GRAPHICS_DRIVER_FAILED error=%d: persistent Metal device/queue/layer not established",error);wonder_game_thread_stop(reason);}
    LOG("GRAPHICS_DRIVER_READY backend=metal3 host Metal device/queue/CAMetalLayer persistent; NVN device not created yet");
}
