#include "../runtime/sdk_services.h"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
int main(void) {
    assert(!wonder_service_ready(WONDER_LOG));
    assert(wonder_service_initialize(WONDER_LOG)!=0); // Config must exist before init.
    assert(wonder_services_configure("/nonexistent-wonder-test/child")!=0);
    char root[]="/tmp/wonder-services-XXXXXX";assert(mkdtemp(root));
    assert(!wonder_services_configure(root));
    for(int i=0;i<WONDER_SERVICE_COUNT;i++) {
        assert(!wonder_service_initialize(i));assert(wonder_service_ready(i));
    }
    assert(!wonder_service_log("test-log-persistence\n"));
    char file[1024];snprintf(file,sizeof file,"%s/sdk-services.log",root);FILE *stream=fopen(file,"r");assert(stream);
    char data[4096];size_t count=fread(data,1,sizeof(data)-1,stream);data[count]=0;fclose(stream);
    assert(strstr(data,"test-log-persistence"));
    snprintf(file,sizeof file,"%s/save",root);struct stat status;assert(!stat(file,&status));assert(S_ISDIR(status.st_mode));
    pid_t child=fork();assert(child>=0);
    if(!child){raise(SIGABRT);_exit(0);}
    int child_status;assert(waitpid(child,&child_status,0)==child);
    assert(WIFSIGNALED(child_status)&&WTERMSIG(child_status)==SIGABRT);
    wonder_services_shutdown();assert(!wonder_service_ready(WONDER_LOG));
    puts("SDK_SERVICES_TEST_OK");
}
