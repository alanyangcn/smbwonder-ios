// Guest fault reporting: SIGSEGV / SIGBUS / SIGILL / SIGTRAP on a guest thread is logged with module offsets
// and the frame-pointer chain, reported as the guest stop, and the faulting thread is parked inside the
// handler (the UI keeps the log; other guest threads park at their next SVC).
// Host-code faults are re-raised unchanged so real host defects still crash. Every guest thread gets an
// alternate signal stack because guest stacks can be a single 4 KiB page.
#include "dm.h"
#include "sysreg_emul.h"
#include <stdatomic.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <dlfcn.h>
#include <sys/ucontext.h>
void wonder_game_report_stop(const char *reason);
static struct sigaction previous[4];
static const int signals[4] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP};
static int index_of(int s) { for (int i = 0; i < 4; i++) if (signals[i] == s) return i; return -1; }
static void describe(char *out, size_t n, uint64_t pc) {
    module_t *m = module_of((void *)pc);
    if (m) snprintf(out, n, "%s+%#llx", m->name, (unsigned long long)(pc - (uint64_t)m->base));
    else snprintf(out, n, "%#llx", (unsigned long long)pc);
}
static void host_describe(char *out, size_t n, uint64_t pc) {   // guest module offset, else host image symbol
    if (module_of((void *)pc)) { describe(out, n, pc); return; }
    Dl_info d;
    if (dladdr((void *)pc, &d) && d.dli_fname) {
        const char *base = strrchr(d.dli_fname, '/'); base = base ? base + 1 : d.dli_fname;
        if (d.dli_sname) snprintf(out, n, "%s+%#llx(%s+%#llx)", base, (unsigned long long)(pc - (uint64_t)d.dli_fbase),
                                  d.dli_sname, (unsigned long long)(pc - (uint64_t)d.dli_saddr));
        else snprintf(out, n, "%s+%#llx", base, (unsigned long long)(pc - (uint64_t)d.dli_fbase));
    } else snprintf(out, n, "%#llx", (unsigned long long)pc);
}
static void handler(int sig, siginfo_t *info, void *context) {
    // kill() from the SDK abort observer (sdk_services.c) re-delivers a fault process-wide; the faulting
    // instruction re-executes and reaches this handler on its own thread, so user-sent copies are ignored.
    if (info->si_code == SI_USER) return;
    ucontext_t *uc = context;
    uint64_t pc = uc->uc_mcontext->__ss.__pc, lr = uc->uc_mcontext->__ss.__lr, fp = uc->uc_mcontext->__ss.__fp;
    if (!module_of((void *)pc)) {   // not guest code: log where (host symbol + frame chain), restore and re-raise
        static _Atomic int logged;
        // Only faults the kernel raised for this instruction (a re-delivered kill() / abort reaches other threads too).
        int raised = (sig == SIGSEGV && (info->si_code == SEGV_MAPERR || info->si_code == SEGV_ACCERR)) ||
                     (sig == SIGBUS && (info->si_code == BUS_ADRALN || info->si_code == BUS_ADRERR || info->si_code == BUS_OBJERR)) || sig == SIGILL || sig == SIGTRAP;
        if (raised && !atomic_exchange(&logged, 1)) {
            char where[160], caller[160]; host_describe(where, sizeof where, pc); host_describe(caller, sizeof caller, lr);
            LOG("HOST_FAULT signal=%d address=%p pc=%s lr=%s sp=%#llx", sig, info->si_addr, where, caller, (unsigned long long)uc->uc_mcontext->__ss.__sp);
            for (unsigned depth = 0; fp && !(fp & 7) && depth < 24; depth++) {
                const uint64_t *frame = (const uint64_t *)fp;
                char ret[160]; host_describe(ret, sizeof ret, frame[1]); LOG("HOST_FAULT_FRAME depth=%u %s", depth, ret);
                if (frame[0] <= fp) break;
                fp = frame[0];
            }
        }
        int i = index_of(sig); if (i >= 0) sigaction(sig, &previous[i], NULL);
        return;                     // returning re-executes the faulting instruction under the old handler
    }
    if (sig == SIGILL) {            // EL0 system-register reads iOS traps but Horizon allows (sysreg_emul.h)
        uint64_t value; int rt = wonder_sysreg_read_emulated(*(const uint32_t *)pc, &value);
        if (rt >= 0) {
            if (rt < 29) uc->uc_mcontext->__ss.__x[rt] = value;   // __x holds x0..x28; x29/x30 are fp/lr; 31 = xzr
            if (rt == 29) uc->uc_mcontext->__ss.__fp = value;
            if (rt == 30) uc->uc_mcontext->__ss.__lr = value;
            uc->uc_mcontext->__ss.__pc = pc + 4;
            static _Atomic unsigned long emulated;
            if (!atomic_fetch_add(&emulated, 1)) {
                char where[96]; describe(where, sizeof where, pc);
                LOG("HOST_SYSREG_EMULATED mrs CTR_EL0 at %s -> %#llx (first; further reads are silent)", where, (unsigned long long)value);
            }
            return;
        }
    }
    char where[96], caller[96]; describe(where, sizeof where, pc); describe(caller, sizeof caller, lr);
    LOG("GUEST_FAULT signal=%d address=%p pc=%s lr=%s sp=%#llx", sig, info->si_addr, where, caller, (unsigned long long)uc->uc_mcontext->__ss.__sp);
    for (unsigned depth = 0; depth < 24 && fp && !(fp & 7); depth++) {
        uint64_t frame[2]; vm_size_t count = 0;
        if (vm_read_overwrite(mach_task_self(), fp, sizeof frame, (vm_address_t)frame, &count) != KERN_SUCCESS || count != sizeof frame) break;
        char ret[96]; describe(ret, sizeof ret, frame[1]); LOG("GUEST_FAULT_FRAME depth=%u %s", depth, ret);
        if (frame[0] <= fp) break; fp = frame[0];
    }
    char reason[256];
    snprintf(reason, sizeof reason, "GUEST_FAULT signal %d at %s (address %p, caller %s)", sig, where, info->si_addr, caller);
    wonder_game_report_stop(reason);   // first stop only; then park this guest thread on its signal stack
    for (;;) pause();
}
void wonder_crash_install(void) {
    struct sigaction a; memset(&a, 0, sizeof a);
    a.sa_sigaction = handler; a.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&a.sa_mask);
    for (int i = 0; i < 4; i++) if (sigaction(signals[i], &a, &previous[i])) die("crash handler install failed");
}
void wonder_crash_thread_init(void) {   // per guest thread: 64 KiB alternate signal stack (leaked with the thread)
    stack_t s = {0}; s.ss_size = 64 * 1024; s.ss_sp = malloc(s.ss_size);
    if (!s.ss_sp || sigaltstack(&s, NULL)) die("sigaltstack failed");
}
