#include "hle.h"
#include "sdk_services.h"
#include <pthread/qos.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
// Whether the calling thread currently runs under THREAD_TIME_CONSTRAINT_POLICY (not the default policy).
static int time_constraint_thread(void){
    thread_time_constraint_policy_data_t p; mach_msg_type_number_t n=THREAD_TIME_CONSTRAINT_POLICY_COUNT; boolean_t def=0;
    mach_port_t self=mach_thread_self();
    kern_return_t kr=thread_policy_get(self,THREAD_TIME_CONSTRAINT_POLICY,(thread_policy_t)&p,&n,&def);
    mach_port_deallocate(mach_task_self(),self);
    return kr==KERN_SUCCESS&&!def;
}
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
    if(rc==EPERM&&time_constraint_thread()){
        // round157: the present thread is a declared time-constraint thread (host_graphics.m rt_thread_tick); Darwin
        // refuses QoS changes on it (EPERM), and its real-time band is above both QoS classes asked for here.
        LOG("HOST_CPU_BOOST mode=%u kept the time-constraint policy (QoS refused on a real-time thread)",mode);return;
    }
    if(rc)die("CPU boost QoS request failed %d",rc);
    LOG("HOST_CPU_BOOST mode=%u QoS applied",mode);
}

// nn::fs::detail::IsEnabledAccessLog(mask) (sdk+0xe7060): when the local target byte matches it asks fsp-srv for the
// global access-log mode (IFileSystemProxy vtable+0x350). Retail systems report mode 0, so access logging is off;
// the host has no fsp-srv and answers exactly that. Callers (e.g. SetPriorityOnCurrentThread, sdk+0xd4344) then
// take their non-logging paths.
HLE_FORCE("_ZN2nn2fs6detail18IsEnabledAccessLogEj","Global fs access-log mode is 0 (retail): no fsp-srv query",uint32_t,wonder_fs_access_log,unsigned mask){(void)mask;return 0;}
