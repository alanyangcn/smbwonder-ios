#include <dlfcn.h>
#include <stdatomic.h>
#include "guest_tls.h"
#include "dm.h"
#include "sdk_services.h"
#include <unistd.h>
#include <mach/mach.h>
extern void dm_gate(void);
extern void wonder_kernel_init(uint64_t);
extern void wonder_kernel_region(uint64_t,uint64_t,uint32_t,uint32_t);
extern int wonder_kernel_call(int,uint64_t *);
#include "kernel_events.h"
static const char *phase="not started";
_Noreturn void wonder_game_thread_stop(const char *reason);
static void *required(const char *name) {
    void *p=guest_sym(name);if(!p)die("SDK missing entry %s",name);return p;
}
// Every SVC number is logged for its first 16 calls (then counted only): with several guest threads the
// synchronization SVCs are hot, and handlers run on guest stacks that may be small.
static _Atomic unsigned svc_seen[256];
int wonder_game_stopped(void);
_Noreturn void wonder_game_thread_park(void);
// Guest frame chain as "main+0x.. main+0x.." from a frame pointer and the link register (KERNEL_WAIT diagnostics).
void wonder_guest_frames(uint64_t fp,uint64_t lr,char *out,size_t size){
    size_t n=0; out[0]=0; uint64_t pc=lr;
    for(unsigned depth=0;depth<16&&n+24<size;depth++){
        module_t *m=module_of((void *)pc);
        if(m) n+=(size_t)snprintf(out+n,size-n,"%s+%#llx ",m->name,(unsigned long long)(pc-(uint64_t)m->base));
        else {   // host code: image + offset (symbol when the image exports one)
            Dl_info d;
            if(dladdr((void *)pc,&d)&&d.dli_fname){const char *b=strrchr(d.dli_fname,'/');b=b?b+1:d.dli_fname;
                if(d.dli_sname)n+=(size_t)snprintf(out+n,size-n,"%s+%#llx(%s) ",b,(unsigned long long)(pc-(uint64_t)d.dli_fbase),d.dli_sname);
                else n+=(size_t)snprintf(out+n,size-n,"%s+%#llx ",b,(unsigned long long)(pc-(uint64_t)d.dli_fbase));}
            else n+=(size_t)snprintf(out+n,size-n,"%#llx ",(unsigned long long)pc);
        }
        uint64_t frame[2]; vm_size_t count=0;
        if(!fp||(fp&15)||vm_read_overwrite(mach_task_self(),fp,sizeof frame,(vm_address_t)frame,&count)!=KERN_SUCCESS||count!=sizeof frame||frame[0]<=fp)break;
        fp=frame[0]; pc=frame[1];
    }
}
uint64_t svc_dispatch(int number,uint64_t *regs,uint64_t original) {
    if(wonder_game_stopped()){LOG("GUEST_PARKED svc=%#x pc=%#llx (guest already stopped)",number,original);wonder_game_thread_park();}
    unsigned seen=svc_seen[number&0xff]++;
    if(seen<16)LOG("SDK_SVC phase=%s nr=%#x pc=%#llx x0=%#llx x1=%#llx x2=%#llx x3=%#llx%s",phase,number,original,regs[0],regs[1],regs[2],regs[3],seen==15?" (further calls not logged)":"");
    uint64_t arg1=regs[1],arg2=regs[2],arg3=regs[3];
    if(wonder_kernel_call(number,regs)){
        static _Atomic unsigned failures;
        if(regs[0]&&atomic_fetch_add(&failures,1)<64)LOG("SDK_SVC_ERROR nr=%#x result=%#llx x1=%#llx x2=%#llx x3=%#llx pc=%#llx",number,(unsigned long long)regs[0],
            (unsigned long long)arg1,(unsigned long long)arg2,(unsigned long long)arg3,(unsigned long long)original);
        return regs[0];
    }
    uint64_t in1=regs[1],in2=regs[2],in29=regs[29],in30=regs[30];
    if(wonder_event_call(number,regs)){
        if(seen<16)LOG("SDK_SVC_RESULT nr=%#x result=%#llx x1=%#llx x2=%#llx",number,regs[0],regs[1],regs[2]);
        // WaitSynchronization on a handle the kernel does not know: show the handles and who waits; a guest spinning
        // on it forever is a host defect, so it stops after a bounded number of retries.
        if(number==0x18&&regs[0]==0xe401){
            static _Atomic unsigned bad_waits;
            unsigned n=atomic_fetch_add(&bad_waits,1);
            if(n<3){
                uint32_t handles[8]={0};vm_size_t count=0;unsigned k=in2<8?(unsigned)in2:8;
                if(k)vm_read_overwrite(mach_task_self(),in1,k*4,(vm_address_t)handles,&count);
                LOG("WAIT_INVALID_HANDLE count=%llu handles=%#x %#x %#x %#x",(unsigned long long)in2,handles[0],handles[1],handles[2],handles[3]);
                uint64_t fp=in29,pc=in30;
                for(unsigned depth=0;depth<12;depth++){
                    module_t *m=module_of((void *)pc);
                    if(m)LOG("WAIT_CALLER depth=%u %s+%#llx",depth,m->name,(unsigned long long)(pc-(uint64_t)m->base));
                    uint64_t frame[2];count=0;
                    if(!fp||(fp&15)||vm_read_overwrite(mach_task_self(),fp,sizeof frame,(vm_address_t)frame,&count)!=KERN_SUCCESS||count!=sizeof frame||frame[0]<=fp)break;
                    fp=frame[0];pc=frame[1];
                }
            }
            if(n==1000){LOG("BOOT_BLOCKED WAIT_INVALID_HANDLE spin");wonder_game_thread_stop("WaitSynchronization spinning on an invalid handle");}
        }
        return regs[0];
    }
    if(number==0x27){fwrite((const void *)regs[0],1,(size_t)regs[1],stderr);fputc('\n',stderr);regs[0]=0;return 0;}
    if(number==0x26 && (regs[0]&0x80000000u))return regs[0];
    if(number==0x26){
        // SDK abort (svc Break): report as the guest stop with the caller chain instead of killing the app.
        uint64_t fp=regs[29],pc=regs[30];
        for(unsigned depth=0;depth<16;depth++){
            module_t *m=module_of((void *)pc);
            if(m)LOG("BREAK_CALLER depth=%u %s+%#llx",depth,m->name,(unsigned long long)(pc-(uint64_t)m->base));
            uint64_t frame[2];vm_size_t count=0;
            if(!fp||(fp&15)||vm_read_overwrite(mach_task_self(),fp,sizeof frame,(vm_address_t)frame,&count)!=KERN_SUCCESS||count!=sizeof frame||frame[0]<=fp)break;
            fp=frame[0];pc=frame[1];
        }
        char reason[160];snprintf(reason,sizeof reason,"SDK_BREAK reason=%#llx during %s at %#llx",(unsigned long long)regs[0],phase,(unsigned long long)original);
        LOG("BOOT_BLOCKED %s",reason);wonder_game_thread_stop(reason);
    }
    // Unknown kernel requests retain their caller chain to identify the service boundary.
    if(number==0x1f) {
        char name[13]={0};vm_size_t count=0;
        if(vm_read_overwrite(mach_task_self(),regs[1],12,(vm_address_t)name,&count)==KERN_SUCCESS)
            LOG("SERVICE_PORT name=%.12s",name);
    }
    uint64_t fp=regs[29],pc=regs[30];
    for(unsigned depth=0;depth<16;depth++) {
        module_t *m=module_of((void *)pc);
        if(m)LOG("BLOCKED_CALLER depth=%u %s+%#llx",depth,m->name,(unsigned long long)(pc-(uint64_t)m->base));
        uint64_t frame[2];vm_size_t count=0;
        if(!fp || (fp&15) || vm_read_overwrite(mach_task_self(),fp,sizeof frame,(vm_address_t)frame,&count)!=KERN_SUCCESS || count!=sizeof frame || frame[0]<=fp)break;
        fp=frame[0];pc=frame[1];
    }
    char reason[256];snprintf(reason,sizeof reason,"unsupported SVC %#x during %s at %#llx",number,phase,(unsigned long long)original);
    LOG("BOOT_BLOCKED %s",reason);
    wonder_game_thread_stop(reason);
}
static void call0(const char *name) {
    phase=name;LOG("SDK_STAGE_BEGIN %s",name);
    ((void (*)(void))required(name))();LOG("SDK_STAGE_OK %s",name);
}
extern void dm_thread_enter(uint64_t,uint64_t,uint64_t);
static void report_line(const char *line){LOG("%s",line);}
// Per guest thread: Horizon TLS region (0x200, TLR) whose +0x1f0 holds the TLS block pointer that
// sdk+0x13651c loads into TPIDR_EL0; the SDK writes its ThreadType at +0x1f8 (sdk+0x131010).
// 8 KiB is diagnostic TLS backing, not a claim that arbitrary guest ELF TLS is supported.
void wonder_crash_thread_init(void);
static void wonder_guest_thread_prepare(void) {
    wonder_crash_thread_init();
    if(wonder_guest_tls_prepare(g_keytls,g_keytlr)) die("guest thread TLS allocation/ownership setup failed");
    pthread_setspecific(g_key18,NULL);pthread_setspecific(g_keygate,(void *)dm_gate);
}
void wonder_sdk_startup(void) {
    uint64_t bytes=0;for(int i=0;i<g_nmod;i++)bytes+=g_mod[i].size;
    wonder_kernel_init(bytes);
    for(int i=0;i<g_nmod;i++) {
        module_t *m=&g_mod[i];uint64_t text=(m->text_end+0x3fff)&~0x3fffull;
        wonder_kernel_region((uint64_t)m->base,text,3,5);
        wonder_kernel_region((uint64_t)m->base+text,m->patch_off-text,4,3);
        wonder_kernel_region((uint64_t)m->base+m->patch_off,m->size-m->patch_off,3,5);
    }
    // Target sdk+0x13651c loads TLR[0x1f0] into TPIDR_EL0; sdk+0x131010 writes [0x1f8].
    // The SDK builds its own ThreadType in module storage; no D2R ThreadType is fabricated here.
    // 8 KiB is diagnostic TLS backing, not a claim that arbitrary guest ELF TLS is supported.
    wonder_guest_thread_prepare();
    uint8_t *tlr=pthread_getspecific(g_keytlr);
    uint32_t main_handle=wonder_threads_register_current(44,0,wonder_guest_thread_prepare,dm_thread_enter,report_line);
    if(main_handle!=0x1001)die("main guest thread handle %#x, expected 0x1001",main_handle);
    uint64_t (*tick)(void)=required("_ZN2nn2os13GetSystemTickEv");
    uint64_t a=tick();usleep(2000);uint64_t b=tick();
    if(b<=a)die("SDK timer gate did not advance");
    LOG("SDK_TIMER_OK delta=%llu",b-a);
    phase="nnosInitialize";LOG("SDK_STAGE_BEGIN %s",phase);
    ((void (*)(uint64_t,uint64_t))required("nnosInitialize"))(0x1001,0);
    LOG("SDK_STAGE_OK %s",phase);
    if(!*(void **)(tlr+0x1f8)||!pthread_getspecific(g_keytls))die("SDK thread/TLS not initialized");
    LOG("SDK_THREAD_OK current=%p TLS=%p",*(void **)(tlr+0x1f8),pthread_getspecific(g_keytls));
    call0("_init_libc1");
    call0("nninitInitializeSdkModule");
    call0("nninitStartup");
    call0("_init_libc2");
    LOG("SDK_INIT_COMPLETE SDK initialization returned");
    // Keep TLS/TLR alive with the persistent diagnostic worker; SDK globals reference them.
}

extern void wonder_module_inits(module_t *,int);
void wonder_game_boot(const char *stage) {
    if(strcmp(stage,"constructors") && strcmp(stage,"main"))die("Invalid post-SDK stage %s",stage);
    phase="global constructors";
    wonder_module_inits(g_mod,g_nmod);
    LOG("CONSTRUCTORS_COMPLETE all module DT_INIT functions returned");
    if(!strcmp(stage,"constructors"))return;
    // Apple/Switch AAPCS64 agree for these fixed integer/pointer arguments.
    phase="host arguments";
    static char program[]="SuperMarioBrosWonder";static char *argv[]={program,NULL};
    ((void (*)(int))required("_ZN2nn2os11SetHostArgcEi"))(1);
    ((void (*)(char **))required("_ZN2nn2os11SetHostArgvEPPc"))(argv);
    phase="nnMain";
    void *entry=required("nnMain");module_t *module=module_of(entry);
    LOG("NNMAIN_ENTER %s+%#llx stack=%#zx",module?module->name:"?",module?(uint64_t)((uint8_t*)entry-module->base):0,pthread_get_stacksize_np(pthread_self()));
    ((void (*)(void))entry)();
    LOG("NNMAIN_RETURNED");
}
