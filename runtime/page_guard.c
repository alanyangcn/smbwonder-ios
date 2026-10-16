#include "page_guard.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
// Ranges in a fixed table (the fault handler never allocates); a page -> watcher-count hash decides when a page may
// become writable for good. One spin lock; the handler takes it too (it never runs inside a guard call on its thread:
// guard calls do not write watched memory).
#define MAX_RANGES (1u << 16)
#define PAGE_SLOTS (1u << 18)
struct range { uintptr_t base, size; _Atomic int dirty; int used; };
static struct range ranges[MAX_RANGES]; static uint32_t next_free = 1, highest;
static struct { uintptr_t page; uint32_t count; } pages[PAGE_SLOTS];   // open addressing, page 0 = empty
static atomic_flag spin = ATOMIC_FLAG_INIT;
static uintptr_t page_size;
static void lock(void) { while (atomic_flag_test_and_set_explicit(&spin, memory_order_acquire)) {} }
static void unlock(void) { atomic_flag_clear_explicit(&spin, memory_order_release); }
static uintptr_t psize(void) { if (!page_size) page_size = (uintptr_t)getpagesize(); return page_size; }
static uint32_t *page_count(uintptr_t page, int create) {
    for (uint32_t i = (uint32_t)((page / psize()) * 2654435761u) & (PAGE_SLOTS - 1), n = 0; n < PAGE_SLOTS; i = (i + 1) & (PAGE_SLOTS - 1), n++) {
        if (pages[i].page == page) return &pages[i].count;
        if (!pages[i].page) { if (!create) return NULL; pages[i].page = page; pages[i].count = 0; return &pages[i].count; }
    }
    return NULL;
}
static void span(const struct range *r, uintptr_t *first, uintptr_t *end) {
    *first = r->base & ~(psize() - 1); *end = (r->base + r->size + psize() - 1) & ~(psize() - 1);
}
uint32_t wonder_guard_watch(const void *base, size_t size) {
    if (!base || !size) return 0;
    lock();
    uint32_t id = 0;
    for (uint32_t i = next_free; i < MAX_RANGES; i++) if (!ranges[i].used) { id = i; break; }
    if (!id) { unlock(); return 0; }
    struct range *r = &ranges[id]; r->base = (uintptr_t)base; r->size = size; atomic_store(&r->dirty, 0); r->used = 1;
    uintptr_t first, end; span(r, &first, &end);
    for (uintptr_t p = first; p < end; p += psize()) { uint32_t *c = page_count(p, 1); if (!c) { r->used = 0; unlock(); return 0; } }
    if (mprotect((void *)first, end - first, PROT_READ)) { r->used = 0; unlock(); return 0; }
    for (uintptr_t p = first; p < end; p += psize()) (*page_count(p, 0))++;
    if (id >= highest) highest = id + 1;
    next_free = id + 1;
    unlock();
    return id;
}
int wonder_guard_dirty(uint32_t id) { return id && id < MAX_RANGES && ranges[id].used ? atomic_load(&ranges[id].dirty) : 1; }
void wonder_guard_rearm(uint32_t id) {
    if (!id || id >= MAX_RANGES) return;
    lock();
    struct range *r = &ranges[id];
    if (r->used) { uintptr_t first, end; span(r, &first, &end); atomic_store(&r->dirty, 0); mprotect((void *)first, end - first, PROT_READ); }
    unlock();
}
void wonder_guard_forget(uint32_t id) {
    if (!id || id >= MAX_RANGES) return;
    lock();
    struct range *r = &ranges[id];
    if (r->used) {
        uintptr_t first, end; span(r, &first, &end);
        for (uintptr_t p = first; p < end; p += psize()) {
            uint32_t *c = page_count(p, 0);
            if (c && *c && !--*c) mprotect((void *)p, psize(), PROT_READ | PROT_WRITE);
        }
        r->used = 0; if (id < next_free) next_free = id;
    }
    unlock();
}
// Holds the lock: every range on [first, end) becomes dirty, the pages writable.
static void open_pages(uintptr_t first, uintptr_t end) {
    for (uint32_t i = 1; i < highest; i++) {
        struct range *r = &ranges[i];
        if (r->used && r->base < end && r->base + r->size > first) atomic_store(&r->dirty, 1);
    }
    mprotect((void *)first, end - first, PROT_READ | PROT_WRITE);
}
void wonder_guard_release(const void *base, size_t size) {
    if (!base || !size) return;
    uintptr_t first = (uintptr_t)base & ~(psize() - 1), end = ((uintptr_t)base + size + psize() - 1) & ~(psize() - 1);
    lock();
    int watched = 0;
    for (uintptr_t p = first; p < end && !watched; p += psize()) { uint32_t *c = page_count(p, 0); watched = c && *c; }
    if (watched) open_pages(first, end);
    unlock();
}
int wonder_guard_fault(uintptr_t address) {
    if (!page_size) return 0;   // nothing was ever watched
    uintptr_t page = address & ~(page_size - 1);
    lock();
    uint32_t *c = page_count(page, 0);
    int handled = c && *c;
    if (handled) open_pages(page, page + page_size);
    unlock();
    return handled;
}
unsigned wonder_guard_count(void) { unsigned n = 0; lock(); for (uint32_t i = 1; i < highest; i++) n += ranges[i].used; unlock(); return n; }
