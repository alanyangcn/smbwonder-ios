#include "access_watch.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static volatile int faults; static char seen[64];
static void handler(int sig, siginfo_t *info, void *ctx) { (void)sig; (void)ctx; if (wonder_access_fault((uintptr_t)info->si_addr, seen, sizeof seen)) { faults++; return; } abort(); }
int main(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    size_t pg = (size_t)getpagesize(); volatile unsigned char *m = aligned_alloc(pg, 2 * pg); m[0] = 7; m[pg] = 9;
    assert(wonder_access_watch((const void *)m, 16, "rt 64x36"));
    assert(m[0] == 7 && faults == 1 && !strcmp(seen, "rt 64x36"));   // first read faults once, then proceeds
    assert(m[1] == 0 && faults == 1);                                  // the page stays open
    assert(m[pg] == 9 && faults == 1);                                 // other pages were never watched
    puts("access_watch OK"); return 0;
}
