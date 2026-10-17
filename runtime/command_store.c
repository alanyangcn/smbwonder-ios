#include "command_store.h"
#include <os/lock.h>
#include <stdlib.h>
#include <string.h>
// Open-addressing hash table keyed by stub address (linear probing; no deletions except a rebuild on forget_range).
struct cmd_entry { uint64_t addr; uint32_t seq, opcode; size_t size; void *payload; };
static struct cmd_entry *table; static size_t capacity, used;
static uint32_t next_seq = 1;
static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;   // round232: short sections, contended by the recording game thread
static size_t slot_of(uint64_t addr, size_t cap) { uint64_t h = addr * UINT64_C(0x9e3779b97f4a7c15); return (size_t)(h >> 20) & (cap - 1); }
static struct cmd_entry *find(struct cmd_entry *t, size_t cap, uint64_t addr) {   // the entry for addr, or the empty slot for it
    for (size_t i = slot_of(addr, cap);; i = (i + 1) & (cap - 1)) if (!t[i].addr || t[i].addr == addr) return &t[i];
}
static int grow(size_t want) {   // holds lock
    size_t cap = capacity ? capacity : 1024;
    while (cap < want * 2) cap *= 2;
    if (cap == capacity) return 0;
    struct cmd_entry *t = calloc(cap, sizeof *t); if (!t) return -1;
    for (size_t i = 0; i < capacity; i++) if (table[i].addr) *find(t, cap, table[i].addr) = table[i];
    free(table); table = t; capacity = cap; return 0;
}
int wonder_cmdstore_put(uint64_t addr, unsigned opcode, const void *payload, size_t size, uint32_t *seq) {
    if (!addr) return -1;
    void *copy = malloc(size ? size : 1); if (!copy) return -1;
    if (size) memcpy(copy, payload, size);
    os_unfair_lock_lock(&lock);
    if (grow(used + 1)) { os_unfair_lock_unlock(&lock); free(copy); return -1; }
    struct cmd_entry *e = find(table, capacity, addr);
    if (e->addr) free(e->payload); else used++;
    uint32_t s = next_seq++; if (!next_seq) next_seq = 1;
    *e = (struct cmd_entry){addr, s, opcode, size, copy};
    os_unfair_lock_unlock(&lock);
    *seq = s; return 0;
}
void *wonder_cmdstore_get(uint64_t addr, uint32_t seq, unsigned *opcode, size_t *size) {
    void *copy = NULL;
    os_unfair_lock_lock(&lock);
    struct cmd_entry *e = capacity ? find(table, capacity, addr) : NULL;
    if (e && e->addr && e->seq == seq && (copy = malloc(e->size ? e->size : 1))) {
        memcpy(copy, e->payload, e->size); *opcode = e->opcode; *size = e->size;
    }
    os_unfair_lock_unlock(&lock);
    return copy;
}
void *wonder_cmdstore_get_into(uint64_t addr, uint32_t seq, unsigned *opcode, size_t *size, void *buffer, size_t capacity_bytes, int *allocated) {
    void *copy = NULL; *allocated = 0;
    os_unfair_lock_lock(&lock);
    struct cmd_entry *e = capacity ? find(table, capacity, addr) : NULL;
    if (e && e->addr && e->seq == seq) {
        if (e->size <= capacity_bytes) copy = buffer; else if ((copy = malloc(e->size))) *allocated = 1;
        if (copy) { memcpy(copy, e->payload, e->size); *opcode = e->opcode; *size = e->size; }
    }
    os_unfair_lock_unlock(&lock);
    return copy;
}
void wonder_cmdstore_forget_range(uint64_t base, uint64_t size) {
    os_unfair_lock_lock(&lock);
    if (capacity) {
        struct cmd_entry *t = calloc(capacity, sizeof *t);
        if (t) {   // rebuild without the range (keeps probe chains intact)
            size_t kept = 0;
            for (size_t i = 0; i < capacity; i++) {
                if (!table[i].addr) continue;
                if (table[i].addr - base < size) { free(table[i].payload); continue; }
                *find(t, capacity, table[i].addr) = table[i]; kept++;
            }
            free(table); table = t; used = kept;
        }
    }
    os_unfair_lock_unlock(&lock);
}
size_t wonder_cmdstore_count(void) { os_unfair_lock_lock(&lock); size_t n = used; os_unfair_lock_unlock(&lock); return n; }
