#include "hle.h"
extern int wonder_applet_enable(unsigned,int);
extern unsigned wonder_applet_get_focus(void);
extern int wonder_applet_pop(uint32_t *);
static void enable(unsigned message,unsigned on) {
    if(wonder_applet_enable(message,(on&0xff)!=0))die("Invalid applet message %u",message);
    LOG("HOST_NOTIFICATION_ENABLE message=%u enabled=%u",message,on&0xff);
}
HLE_FORCE("_ZN2nn2oe28SetResumeNotificationEnabledEb","Store resume subscription; UIKit foreground transition emits message 16",void,h_resume,unsigned on){enable(16,on);}
HLE_FORCE("_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb","Store operation-mode subscription; host remains handheld",void,h_operation,unsigned on){enable(30,on);}
HLE_FORCE("_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb","Store performance-mode subscription; host remains normal mode",void,h_performance,unsigned on){enable(31,on);}
HLE_FORCE("_ZN2nn2oe25TryPopNotificationMessageEPj","Drain host applet notification queue",unsigned,h_pop,uint32_t *message){return wonder_applet_pop(message);}
HLE_FORCE("_ZN2nn2oe20GetCurrentFocusStateEv","Return focus driven by UIKit lifecycle",unsigned,h_focus,void){return wonder_applet_get_focus();}
HLE_FORCE("_ZN2nn2oe16GetOperationModeEv","iPhone host is handheld",unsigned,h_get_operation,void){return 0;}
HLE_FORCE("_ZN2nn2oe18GetPerformanceModeEv","Host normal mode; CPU requests use QoS rather than console hardware clocks",unsigned,h_get_performance,void){return 0;}
extern uint64_t wonder_host_language(void);
HLE_FORCE("_ZN2nn2oe18GetDesiredLanguageEv","Convert actual iOS preferred language to console language code",uint64_t,h_language,void){uint64_t code=wonder_host_language();LOG("HOST_LANGUAGE code=%.8s",(char*)&code);return code;}

// ---------- Remaining nn::oe application functions (am ISelfController/IApplicationFunctions on the host) ----------
#include "sdk_services.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
static pthread_mutex_t oe_lock = PTHREAD_MUTEX_INITIALIZER;
// control.nacp DisplayVersion (+0x3060) of update 262144, verified from work/game/control-update/Acd_1/control.nacp.
#define WONDER_DISPLAY_VERSION "1.2.0"
HLE_FORCE("_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE","Display version from the verified update control.nacp",void,h_display_version,char *out){
    if(!out)die("oe::GetDisplayVersion out=NULL");
    memset(out,0,0x10);strcpy(out,WONDER_DISPLAY_VERSION);
}
// Pseudo device id: stable per device and application, never the hardware id. Generated once and kept beside the
// host account data (WonderSDK/oe/pseudo_device_id.bin).
HLE_FORCE("_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE","Stable per-install random id persisted in the app container",void,h_pseudo_device_id,uint8_t *out){
    if(!out)die("oe::GetPseudoDeviceId out=NULL");
    static uint8_t id[16];static int loaded;
    pthread_mutex_lock(&oe_lock);
    if(!loaded){
        const char *root=wonder_services_root();char dir[PATH_MAX],path[PATH_MAX];
        if(!root||snprintf(dir,sizeof dir,"%s/oe",root)>=(int)sizeof dir)die("oe pseudo device id: no host data root");
        if(mkdir(dir,0700)&&errno!=EEXIST)die("oe pseudo device id: mkdir %s errno=%d",dir,errno);
        snprintf(path,sizeof path,"%s/pseudo_device_id.bin",dir);
        FILE *f=fopen(path,"rb");
        if(!f||fread(id,1,16,f)!=16){
            if(f)fclose(f);
            arc4random_buf(id,16);id[6]=(uint8_t)((id[6]&0x0f)|0x40);id[8]=(uint8_t)((id[8]&0x3f)|0x80);   // RFC 4122 version 4
            char tmp[PATH_MAX];snprintf(tmp,sizeof tmp,"%s.tmp",path);
            f=fopen(tmp,"wb");
            if(!f||fwrite(id,1,16,f)!=16||fclose(f)||rename(tmp,path))die("oe pseudo device id: cannot persist %s",path);
        } else fclose(f);
        loaded=1;
    }
    memcpy(out,id,16);
    pthread_mutex_unlock(&oe_lock);
}
// FocusHandlingMode: whether the app is suspended or notified when it loses focus. UIKit decides suspension on
// iOS; the mode is kept and the focus notifications keep following the UIKit lifecycle.
static atomic_int focus_handling_mode=1;   // nn::oe default: Notify
HLE_FORCE("_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE","Keep the mode; focus follows the UIKit lifecycle",void,h_focus_mode,int mode){
    if(mode<0||mode>3)die("oe::SetFocusHandlingMode mode=%d",mode);
    atomic_store(&focus_handling_mode,mode);LOG("HOST_OE_FOCUS_HANDLING mode=%d",mode);
}
// Expected volume balance (application, library applets): stored values, default full scale.
static float volume_app=1.0f,volume_applet=1.0f;
HLE_FORCE("_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_","Stored expected volume balance",void,h_get_volume_balance,float *app,float *applet){
    if(!app||!applet)die("oe::GetExpectedVolumeBalance NULL");
    pthread_mutex_lock(&oe_lock);*app=volume_app;*applet=volume_applet;pthread_mutex_unlock(&oe_lock);
}
HLE_FORCE("_ZN2nn2oe24SetExpectedVolumeBalanceEff","Stored expected volume balance",void,h_set_volume_balance,float app,float applet){
    if(!(app>=0.f&&app<=1.f)||!(applet>=0.f&&applet<=1.f))die("oe::SetExpectedVolumeBalance %f %f",app,applet);
    pthread_mutex_lock(&oe_lock);volume_app=app;volume_applet=applet;pthread_mutex_unlock(&oe_lock);
    LOG("HOST_OE_VOLUME_BALANCE application=%.3f applet=%.3f",app,applet);
}
static struct timespec process_start;
__attribute__((constructor)) static void record_process_start(void){clock_gettime(CLOCK_MONOTONIC,&process_start);}
// Program total active time: nn::TimeSpan (ns) since the program started on this host.
HLE_FORCE("_ZN2nn2oe25GetProgramTotalActiveTimeEv","Active time since process start (host monotonic clock)",int64_t,h_active_time,void){
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    return (int64_t)(now.tv_sec-process_start.tv_sec)*1000000000+(now.tv_nsec-process_start.tv_nsec);
}
// Performance configuration (CPU/GPU/memory clocks per mode): no console clocks on iOS; recorded only.
HLE_FORCE("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi","Console clock configuration has no iOS equivalent; recorded",void,h_perf_config,int mode,int config){
    if(mode<0||mode>1)die("oe::SetPerformanceConfiguration mode=%d",mode);
    LOG("HOST_OE_PERFORMANCE_CONFIGURATION mode=%d config=%#x",mode,(unsigned)config);
}
// Exit-request handling sections: iOS cannot defer process termination, so there is no exit request to hold back;
// the nesting count is kept and must balance (Leave without Enter stops).
static atomic_int exit_section_depth;
HLE_FORCE("_ZN2nn2oe31EnterExitRequestHandlingSectionEv","Nesting count; iOS issues no deferrable exit requests",void,h_enter_exit_section,void){
    int d=atomic_fetch_add(&exit_section_depth,1)+1;if(d==1)LOG("HOST_OE_EXIT_SECTION enter");
}
HLE_FORCE("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv","Nesting count; iOS issues no deferrable exit requests",void,h_leave_exit_section,void){
    int d=atomic_fetch_sub(&exit_section_depth,1)-1;if(d<0)die("oe::LeaveExitRequestHandlingSection without Enter");
    if(d==0)LOG("HOST_OE_EXIT_SECTION leave");
}
// Extended user-inactivity detection time: the console's auto-sleep is iOS's idle timer; recorded.
HLE_FORCE("_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb","Recorded; auto-lock belongs to iOS",void,h_inactivity,unsigned extended){
    LOG("HOST_OE_INACTIVITY_EXTENDED %u",extended&0xff);
}
