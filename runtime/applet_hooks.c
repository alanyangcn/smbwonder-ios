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
