#include "access_watch.h"
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define MAX 256
static struct { uintptr_t first, end; char label[48]; } watch[MAX];
static _Atomic unsigned count;
static uintptr_t page(void) { static uintptr_t p; if (!p) p = (uintptr_t)getpagesize(); return p; }
int wonder_access_watch(const void *base, size_t size, const char *label) {
    if (!base || !size) return 0;
    uintptr_t first = (uintptr_t)base & ~(page() - 1), end = ((uintptr_t)base + size + page() - 1) & ~(page() - 1);
    unsigned i = atomic_fetch_add(&count, 1); if (i >= MAX) return 0;
    watch[i].first = first; watch[i].end = end; strncpy(watch[i].label, label ? label : "", sizeof watch[i].label - 1);
    return mprotect((void *)first, end - first, PROT_NONE) == 0;
}
int wonder_access_fault(uintptr_t address, char *label, size_t n) {
    unsigned c = atomic_load(&count); if (c > MAX) c = MAX;
    for (unsigned i = 0; i < c; i++) if (address >= watch[i].first && address < watch[i].end) {
        uintptr_t p = address & ~(page() - 1);
        mprotect((void *)p, page(), PROT_READ | PROT_WRITE);
        if (label && n) { strncpy(label, watch[i].label, n - 1); label[n - 1] = 0; }
        return 1;
    }
    return 0;
}
