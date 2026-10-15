#include "hle.h"
#include "sdk_services.h"
#include <pthread/qos.h>
static void initialize(enum wonder_service service,const char *name) {
    int error=wonder_service_initialize(service);if(error)die("HOST_SERVICE_FAILED %s error=%d",name,error);
    LOG("HOST_SERVICE_READY %s (subsequent unimplemented requests remain fatal)",name);
}
HLE_FORCE("nndiagStartup","Host persistent diagnostic logger instead of Horizon lm session",void,wonder_log_init,void){initialize(WONDER_LOG,"logging");}
HLE_FORCE("_ZN2nn2oe10InitializeEv","Host process environment instead of Horizon oe session",void,wonder_oe_init,void){initialize(WONDER_OE,"environment");}
HLE_FORCE("_ZN2nn3aoc10InitializeEv","Initialize empty mounted-DLC catalogue: input contains base/update only",void,wonder_aoc_init,void){initialize(WONDER_AOC,"DLC catalogue");}
HLE_FORCE("_ZN2nn3err26InitializeContextWriterApiEv","Allocate host per-thread error context",void,wonder_error_init,void){initialize(WONDER_ERROR,"error context");}
HLE_FORCE("_ZN2nn2fs24InitializeForApplicationEv","Create independent host filesystem context; mount and file APIs pending",void,wonder_fs_init,void){initialize(WONDER_FS,"filesystem");}
HLE_FORCE("_ZN2nn4pctl10InitializeEv","Initialize host policy with online access denied; Nintendo policy APIs pending",void,wonder_parental_init,void){initialize(WONDER_PARENTAL,"access policy");}
HLE_FORCE("_ZN2nn4diag34InitializeApplicationAbortObserverEv","Install fatal-signal observer that preserves original termination",void,wonder_abort_init,void){initialize(WONDER_ABORT,"abort observer");}
HLE_FORCE("_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE","Map loading CPU request to host thread QoS; Apple controls hardware clocks",void,wonder_cpu_boost,unsigned mode){
    if(!wonder_service_ready(WONDER_OE)||mode>1)die("Invalid CPU boost request %u",mode);
    int rc=pthread_set_qos_class_self_np(mode?QOS_CLASS_USER_INTERACTIVE:QOS_CLASS_USER_INITIATED,0);
    if(rc)die("CPU boost QoS request failed %d",rc);
    LOG("HOST_CPU_BOOST mode=%u QoS applied",mode);
}
