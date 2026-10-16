#include "page_guard.h"
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static int faults;
static void handler(int sig, siginfo_t *info, void *ctx) {
    (void)sig; (void)ctx;
    if (wonder_guard_fault((uintptr_t)info->si_addr)) { faults++; return; }   // retried after the page opens
    _exit(9);
}
int main(void) {
    struct sigaction a; memset(&a, 0, sizeof a); a.sa_sigaction = handler; a.sa_flags = SA_SIGINFO; sigemptyset(&a.sa_mask);
    sigaction(SIGSEGV, &a, NULL); sigaction(SIGBUS, &a, NULL);
    size_t ps = (size_t)getpagesize();
    unsigned char *m = mmap(NULL, 4 * ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); assert(m != MAP_FAILED);
    uint32_t t1 = wonder_guard_watch(m + ps + 100, 200), t2 = wonder_guard_watch(m + ps + 1000, 100);   // two textures, one page
    assert(t1 && t2 && t1 != t2 && !wonder_guard_dirty(t1) && !wonder_guard_dirty(t2));
    m[0] = 1; m[3 * ps] = 1; assert(faults == 0);              // unwatched pages write freely
    volatile unsigned char x = m[ps + 150]; (void)x; assert(faults == 0 && !wonder_guard_dirty(t1));   // reads never fault
    m[ps + 2000] = 7; assert(faults == 1 && m[ps + 2000] == 7);   // same page, outside both ranges: page granularity
    assert(wonder_guard_dirty(t1) && wonder_guard_dirty(t2));
    m[ps + 150] = 3; assert(faults == 1);                       // page stays open until rearmed
    wonder_guard_rearm(t1); assert(!wonder_guard_dirty(t1) && wonder_guard_dirty(t2));
    m[ps + 150] = 4; assert(faults == 2 && wonder_guard_dirty(t1) && m[ps + 150] == 4);
    // A host syscall writing into a watched page: released first (read() would fail with EFAULT otherwise).
    wonder_guard_rearm(t1); wonder_guard_rearm(t2);
    int fd = open("/dev/zero", O_RDONLY); assert(fd >= 0);
    wonder_guard_release(m + ps + 100, 16); assert(wonder_guard_dirty(t1) && read(fd, m + ps + 100, 16) == 16 && m[ps + 100] == 0);
    close(fd);
    // Forget: once no range watches the page it stays writable.
    wonder_guard_rearm(t1); wonder_guard_forget(t1); assert(wonder_guard_count() == 1);
    wonder_guard_forget(t2); assert(wonder_guard_count() == 0);
    int before = faults; m[ps + 150] = 5; m[ps + 1050] = 6; assert(faults == before);
    assert(!wonder_guard_watch(NULL, 10) && !wonder_guard_watch(m, 0));
    puts("PAGE_GUARD_OK");
}
