#include "hle.h"
#include "vi_state.h"
#include "kernel_events.h"
extern void *wonder_host_native_layer(void);
extern int wonder_host_register_vsync(uint32_t);
static void *required(const char *name){void *p=guest_sym(name);if(!p)die("VI missing SDK entry %s",name);return p;}
HLE_FORCE("_ZN2nn2vi10InitializeEv","Initialize display registry backed by a real CAMetalLayer",void,h_vi_initialize,void){
 void *layer=wonder_host_native_layer();if(wonder_vi_initialize(layer,1280,720))die("VI host display unavailable");LOG("VI_INITIALIZE native_layer=%p logical=1280x720",layer);
}
HLE_FORCE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE","Open registered host display",Result,h_vi_open,void **out){Result r=wonder_vi_open(out);LOG("VI_OPEN_DISPLAY result=%#x",r);return r;}
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE","Create the supported host Metal layer with handle validation",Result,h_vi_create,void **out,void *display){Result r=wonder_vi_create(out,display);LOG("VI_CREATE_LAYER result=%#x",r);return r;}
HLE_FORCE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE","Expose registered CAMetalLayer to host NVN window",Result,h_vi_native,void **out,void *layer){return wonder_vi_native(out,layer);}
HLE_FORCE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE","Native SDK SystemEvent signaled by actual CADisplayLink ticks",Result,h_vi_vsync,void *event,void *display){
 if(!event||!wonder_vi_valid_display(display))return 0xe401;
 uint32_t (*create)(void*,int,unsigned)=required("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
 uint32_t (*getw)(const void*)=required("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE");
 Result r=create(event,1,1);if(r)return r;uint32_t writable=getw(event);
 if(wonder_host_register_vsync(writable))die("VI vsync registration failed");LOG("VI_VSYNC_READY event=%p writable=%#x source=CADisplayLink",event,writable);return 0;
}
