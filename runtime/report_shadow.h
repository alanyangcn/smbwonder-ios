#pragma once
// GPU writes to memory the replay recorded since it last finished the GPU (round199/200): ReportValue stores a constant
// 32-bit value; timestamps, buffer clears, uniform updates and shader storage writes store bytes the CPU does not track
// (unknown). A conditional render (one CPU/GPU round trip per conditional, ~50 per frame in a course, when it always
// finished the GPU) reads its records from memory when no pending write touches them, from here when only value
// writes do, and finishes the GPU otherwise. Cleared only after the GPU was finished.
#include <stdint.h>
#define WONDER_REPORT_SHADOW_SLOTS 4096
struct wonder_report_shadow {
    unsigned count; int overflow;
    struct { uint64_t address, size; uint32_t value; int known; } entry[WONDER_REPORT_SHADOW_SLOTS];
};
void wonder_report_shadow_clear(struct wonder_report_shadow *s);
void wonder_report_shadow_value(struct wonder_report_shadow *s, uint64_t address, uint32_t value);     // 4-byte value write
void wonder_report_shadow_unknown(struct wonder_report_shadow *s, uint64_t address, uint64_t size);   // write of unknown bytes
// The 4 bytes at address: 1 = a pending value write stores *value there (the latest one), 0 = no pending write touches
// them, -1 = a pending write of unknown bytes overlaps them (or the table overflowed).
int wonder_report_shadow_lookup(const struct wonder_report_shadow *s, uint64_t address, uint32_t *value);
