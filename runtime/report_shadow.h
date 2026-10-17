#pragma once
// Pending report writes of the replay (round199): ReportValue commands write a constant 32-bit value at their GPU
// position. A conditional render compares report records the same command stream wrote earlier; instead of finishing
// the GPU to read them back (one CPU/GPU round trip per conditional, ~50 per frame in a course), the replay keeps what
// each pending write will store. Writes whose value is not known in advance (timestamps) mark their range unknown.
#include <stdint.h>
#define WONDER_REPORT_SHADOW_SLOTS 512
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
