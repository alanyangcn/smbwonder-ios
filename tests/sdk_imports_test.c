#include "dm.h"
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
struct dm_flags g_flag;
uint64_t unimpl_stubs[4096];
void *hlecount_wrap(const char *kind,const char *name,void *p) {(void)kind;(void)name;return p;}
uint64_t guest_ns(void) {return 0;}
uint64_t svc_dispatch(int n,uint64_t *r,uint64_t o) {(void)n;(void)r;(void)o;abort();}
extern uint64_t resolve_import(module_t *,const char *);
extern uint64_t unimpl_c(unsigned,uint64_t *);
int main(void) {
    Elf64_Sym syms[2]={{0},{.st_name=1,.st_info=0x20}};
    module_t m={.symtab=syms,.strtab="\0weak\0",.nsym=2};
    assert(resolve_import(&m,"weak")==0);
    uint64_t strong=resolve_import(&m,"missing_strong");assert(strong==(uint64_t)&unimpl_stubs[0]);
    pid_t child=fork();assert(child>=0);
    if(!child){uint64_t r[31]={0};unimpl_c(0,r);_exit(0);}
    int status=0;assert(waitpid(child,&status,0)==child);assert(WIFSIGNALED(status));assert(WTERMSIG(status)==SIGABRT);
    puts("SDK_IMPORTS_TEST_OK");
}
