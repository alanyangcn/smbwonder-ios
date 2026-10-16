#pragma once
// Write-watching of guest memory that the host caches (sampled textures uploaded from pool storage). Watched pages are
// made read-only; the first CPU write faults, the fault handler (crash_report.c) marks every range on that page dirty and
// makes the page writable again, and the write is retried. The cache re-validates only dirty ranges, instead of hashing
// every texture on every draw (round141: hundreds of 512x512 textures hashed per draw, ~5 fps in a course).
// Host system calls that write guest memory (romfs / save reads) release their destination first (a protected page would
// fail the syscall with EFAULT instead of faulting). GPU writes do not fault: ranges written by the GPU are not covered.
#include <stddef.h>
#include <stdint.h>
// Watch [base, base + size): returns an id > 0, or 0 when the pages cannot be protected (the caller keeps hashing).
uint32_t wonder_guard_watch(const void *base, size_t size);
int wonder_guard_dirty(uint32_t id);        // 1 = written since watch / rearm
void wonder_guard_rearm(uint32_t id);       // clear dirty and protect the pages again
void wonder_guard_forget(uint32_t id);      // stop watching; pages no range watches become writable
void wonder_guard_release(const void *base, size_t size);   // before a host syscall writes here: writable + dirty
int wonder_guard_fault(uintptr_t address);  // from the SIGSEGV/SIGBUS handler: 1 = a watched page, handled (retry)
unsigned wonder_guard_count(void);
