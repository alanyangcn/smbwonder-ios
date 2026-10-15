// Applet messages and focus values: switchbrew/libnx services/applet.h.
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned focus=1;
static int resume_enabled,operation_enabled,performance_enabled;
static uint32_t queue[8];static unsigned count;
static void enqueue(uint32_t message) {
    // These messages invalidate queryable state; preserve first position and coalesce duplicates.
    for(unsigned i=0;i<count;i++)if(queue[i]==message)return;
    if(count<8)queue[count++]=message;
}
int wonder_applet_enable(unsigned message,int enabled) {
    pthread_mutex_lock(&lock);
    int *flag=message==16?&resume_enabled:message==30?&operation_enabled:message==31?&performance_enabled:NULL;
    if(flag)*flag=!!enabled;
    pthread_mutex_unlock(&lock);return flag?0:EINVAL;
}
void wonder_applet_focus(unsigned state) {
    if(state<1||state>3)return;
    pthread_mutex_lock(&lock);
    unsigned previous=focus;
    if(state!=previous){focus=state;enqueue(15);if(state==1&&previous==3&&resume_enabled)enqueue(16);}
    pthread_mutex_unlock(&lock);
}
unsigned wonder_applet_get_focus(void) {
    pthread_mutex_lock(&lock);unsigned value=focus;pthread_mutex_unlock(&lock);return value;
}
int wonder_applet_pop(uint32_t *message) {
    if(!message)return 0;
    pthread_mutex_lock(&lock);
    int available=count!=0;
    if(available){*message=queue[0];for(unsigned i=1;i<count;i++)queue[i-1]=queue[i];count--;}
    pthread_mutex_unlock(&lock);return available;
}

#include <string.h>
#include <strings.h>
// iOS BCP-47 locale -> console's eight-byte, zero-padded language code.
uint64_t wonder_language_code(const char *locale) {
    const char *code="en-US";if(!locale)locale="";
    const struct {const char *prefix,*code;} specific[]={
        {"zh-Hant","zh-Hant"},{"zh-TW","zh-Hant"},{"zh-HK","zh-Hant"},
        {"zh","zh-Hans"},{"en-GB","en-GB"},{"fr-CA","fr-CA"},{"es-MX","es-419"},{"es-419","es-419"},
        {"pt-BR","pt-BR"},{"en","en-US"},{"ja","ja"},{"fr","fr"},{"de","de"},{"es","es"},
        {"it","it"},{"ko","ko"},{"nl","nl"},{"pt","pt"},{"ru","ru"}};
    for(unsigned i=0;i<sizeof specific/sizeof specific[0];i++){
        size_t n=strlen(specific[i].prefix);
        if(!strncasecmp(locale,specific[i].prefix,n)&&(locale[n]==0||locale[n]=='-'||locale[n]=='_')){code=specific[i].code;break;}
    }
    uint64_t value=0;memcpy(&value,code,strlen(code));return value;
}
