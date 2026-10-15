#include "texture_storage.h"
#include <stdlib.h>
#include <string.h>
struct entry { struct wonder_storage_key key; void *object; void (*release)(void *); };
static struct entry *entries; static unsigned count, capacity;
static int same_desc(const nvn_texdesc *a, const nvn_texdesc *b) {
    return a->target == b->target && a->format == b->format && a->flags == b->flags && a->samples == b->samples && a->width == b->width &&
           a->height == b->height && a->depth == b->depth && a->levels == b->levels && a->stride == b->stride && a->pkg_block_h == b->pkg_block_h;
}
void *wonder_storage_find(const struct wonder_storage_key *k) {
    for (unsigned i = 0; i < count; i++) {
        const struct wonder_storage_key *e = &entries[i].key;
        if (e->pool == k->pool && e->offset == k->offset && e->size == k->size && same_desc(&e->desc, &k->desc)) return entries[i].object;
    }
    return NULL;
}
static void remove_at(unsigned i) { if (entries[i].release) entries[i].release(entries[i].object); entries[i] = entries[--count]; }
void wonder_storage_insert(const struct wonder_storage_key *k, void *object, void (*release)(void *)) {
    for (unsigned i = 0; i < count;) {
        const struct wonder_storage_key *e = &entries[i].key;
        if (e->pool == k->pool && e->offset < k->offset + (int64_t)k->size && k->offset < e->offset + (int64_t)e->size) remove_at(i); else i++;
    }
    if (count == capacity) {
        unsigned grown = capacity ? capacity * 2 : 64; struct entry *p = realloc(entries, grown * sizeof *p);
        if (!p) abort();
        entries = p; capacity = grown;
    }
    entries[count++] = (struct entry){*k, object, release};
}
void wonder_storage_forget_pool(const void *pool) { for (unsigned i = 0; i < count;) if (entries[i].key.pool == pool) remove_at(i); else i++; }
unsigned wonder_storage_count(void) { return count; }
