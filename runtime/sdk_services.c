// Host-owned initialization contexts, replacing Horizon service sessions.
// This does not implement the subsequent Nintendo service APIs. Unknown IPC still stops.
#include "sdk_services.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static char service_root[PATH_MAX];
static int initialized[WONDER_SERVICE_COUNT],log_fd=-1;
static pthread_key_t error_key;
static int error_key_valid;
static struct {pid_t process;int dlc_count;int online_allowed;} environment;
static struct sigaction previous[3];
static const int observed_signals[]={SIGABRT,SIGBUS,SIGSEGV};
static void observe_abort(int number) {
    static const char message[]="SDK_HOST_ABORT_OBSERVER: fatal signal\n";
    if(log_fd>=0)(void)write(log_fd,message,sizeof message-1);
    (void)write(STDERR_FILENO,message,sizeof message-1);
    // Restore the previous disposition and re-deliver: observation must not swallow crashes.
    for(unsigned i=0;i<3;i++)if(observed_signals[i]==number){sigaction(number,&previous[i],NULL);break;}
    kill(getpid(),number);
}
int wonder_services_configure(const char *root) {
    if(!root||strlen(root)>=sizeof service_root)return EINVAL;
    struct stat s;if(stat(root,&s)||!S_ISDIR(s.st_mode))return ENOENT;
    if(service_root[0])return EALREADY;
    strcpy(service_root,root);return 0;
}
const char *wonder_services_root(void) { return service_root[0] ? service_root : NULL; }
int wonder_service_ready(enum wonder_service service) {
    return service>=0&&service<WONDER_SERVICE_COUNT&&initialized[service];
}
int wonder_service_log(const char *message) {
    if(log_fd<0)return ENODEV;
    const char *p=message;size_t left=strlen(p);
    while(left){ssize_t n=write(log_fd,p,left);if(n<0){if(errno==EINTR)continue;return errno;}p+=n;left-=n;}
    return 0;
}
int wonder_service_initialize(enum wonder_service service) {
    if(!service_root[0])return ENODEV;
    if(service<0||service>=WONDER_SERVICE_COUNT)return EINVAL;
    if(initialized[service])return 0;
    char path[PATH_MAX];
    switch(service) {
    case WONDER_LOG:
        if(snprintf(path,sizeof path,"%s/sdk-services.log",service_root)>=(int)sizeof path)return ENAMETOOLONG;
        log_fd=open(path,O_CREAT|O_APPEND|O_WRONLY,0600);if(log_fd<0)return errno;break;
    case WONDER_OE:
        environment.process=getpid();break; // Host process identity; actual CPU policy is set by the paired hook.
    case WONDER_AOC:
        environment.dlc_count=0;break; // Only base+update supplied; no DLC mounted by this host.
    case WONDER_ERROR: {
        int rc=pthread_key_create(&error_key,free);if(rc)return rc;
        error_key_valid=1;void *context=calloc(1,4096);if(!context)return ENOMEM;
        rc=pthread_setspecific(error_key,context);if(rc){free(context);return rc;}break;
    }
    case WONDER_FS:
        for(unsigned i=0;i<2;i++) {
            if(snprintf(path,sizeof path,"%s/%s",service_root,i?"temp":"save")>=(int)sizeof path)return ENAMETOOLONG;
            if(mkdir(path,0700)&&errno!=EEXIST)return errno;
            struct stat s;if(stat(path,&s)||!S_ISDIR(s.st_mode))return ENOTDIR;
        }break;
    case WONDER_PARENTAL:
        environment.online_allowed=0;break; // No Nintendo account policy available: online operations are denied.
    case WONDER_ABORT: {
        struct sigaction action={0};action.sa_handler=observe_abort;sigemptyset(&action.sa_mask);
        for(unsigned i=0;i<3;i++) { if(sigaction(observed_signals[i],&action,&previous[i])){
            int error=errno;while(i){--i;sigaction(observed_signals[i],&previous[i],NULL);}return error;
        }}
        break;
    }
    default:return EINVAL;
    }
    initialized[service]=1;
    if(log_fd>=0){char message[80];snprintf(message,sizeof message,"HOST_SERVICE_READY %d\n",service);return wonder_service_log(message);}
    return 0;
}
void wonder_services_shutdown(void) {
    if(initialized[WONDER_ABORT])for(unsigned i=0;i<3;i++)sigaction(observed_signals[i],&previous[i],NULL);
    if(error_key_valid){free(pthread_getspecific(error_key));pthread_setspecific(error_key,NULL);pthread_key_delete(error_key);error_key_valid=0;}
    if(log_fd>=0){close(log_fd);log_fd=-1;}
    memset(initialized,0,sizeof initialized);memset(service_root,0,sizeof service_root);
}
