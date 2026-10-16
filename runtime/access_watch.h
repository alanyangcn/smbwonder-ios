#pragma once
// Diagnostic read/write watch of guest memory (round180): the pages of [base, base + size) become inaccessible; the
// first CPU access faults, the handler (crash_report.c) logs who touched it and makes the page accessible again.
// Used to see whether the game reads render-target memory on the CPU (GPU writes go to Metal textures, not there).
#include <stddef.h>
#include <stdint.h>
int wonder_access_watch(const void *base, size_t size, const char *label);   // 1 armed, 0 not (full table / mprotect)
// From the SIGSEGV/SIGBUS handler: 1 when address is on a watched page (the page is opened; label set), else 0.
int wonder_access_fault(uintptr_t address, char *label, size_t n);
