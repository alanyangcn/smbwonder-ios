#pragma once
// Out-of-line NVN command records. Guest command memory holds one 8-byte stub per command
// {WONDER_NVN_STUB_MAGIC, seq}; the record itself (opcode + payload) lives here, keyed by the stub's guest address.
// Why: the game sizes command memory from GetCommandMemoryUsed and budgets it in exact-fit resource heaps
// (round125: 63 pre-recorded shape lists of Model/PlayerYoshi.bfres made its 48 KiB GPU block miss the heap by a few
// KiB with full in-line host records). 8 bytes is never more than an NVN command (a method header plus a data word).
// Re-recording an address replaces its record (the old one is gone, as overwritten guest bytes would be); pool
// finalization forgets the pool's range. Thread-safe.
#include <stddef.h>
#include <stdint.h>
#define WONDER_NVN_STUB_MAGIC UINT32_C(0x53434d57)   // "WMCS"
#define WONDER_NVN_STUB_SIZE 8
// Stores a copy of payload for the stub at addr; *seq receives the stub's sequence number. 0 ok, -1 out of memory.
int wonder_cmdstore_put(uint64_t addr, unsigned opcode, const void *payload, size_t size, uint32_t *seq);
// Copy of the record for the stub at addr (caller frees), or NULL when there is none or seq does not match.
void *wonder_cmdstore_get(uint64_t addr, uint32_t seq, unsigned *opcode, size_t *size);
// Same, copied into buffer when the record fits capacity_bytes (*allocated = 0), else into a malloc'd copy the caller
// frees (*allocated = 1); NULL as above (round232: one malloc/free per replayed command was ~10% of the replay).
void *wonder_cmdstore_get_into(uint64_t addr, uint32_t seq, unsigned *opcode, size_t *size, void *buffer, size_t capacity_bytes, int *allocated);
void wonder_cmdstore_forget_range(uint64_t base, uint64_t size);
size_t wonder_cmdstore_count(void);
