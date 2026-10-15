// GPU textures are identified by their storage (pool, offset, layout), as on the hardware: Wonder re-creates its
// transient textures every frame at the same pool offsets (round102: 9 textures x ~1820 presents) and expects the
// memory's contents to carry over.
#include "texture_storage.h"
#include <assert.h>
#include <stdio.h>
static int released[8];
static void release(void *object) { released[(int)(long)object]++; }
static struct wonder_storage_key key(const void *pool, int64_t offset, uint64_t size, uint32_t w, int format) {
    struct wonder_storage_key k = {pool, offset, size, {0}}; k.desc.target = 1; k.desc.format = format; k.desc.width = w; k.desc.height = w; k.desc.depth = 1; k.desc.levels = 1;
    return k;
}
int main(void) {
    static int pool_a, pool_b;
    struct wonder_storage_key a = key(&pool_a, 0x8400, 0x258000, 960, 0x3f);
    assert(!wonder_storage_find(&a));
    wonder_storage_insert(&a, (void *)1, release);
    struct wonder_storage_key again = key(&pool_a, 0x8400, 0x258000, 960, 0x3f);       // next frame's texture object
    assert(wonder_storage_find(&again) == (void *)1 && wonder_storage_count() == 1);
    struct wonder_storage_key other_pool = key(&pool_b, 0x8400, 0x258000, 960, 0x3f);
    assert(!wonder_storage_find(&other_pool));
    wonder_storage_insert(&other_pool, (void *)2, release);
    struct wonder_storage_key reinterpreted = key(&pool_a, 0x8400, 0x258000, 960, 0x35);   // same memory, other format
    assert(!wonder_storage_find(&reinterpreted));
    // A texture laid over part of an existing one replaces it: the memory now holds the new texture.
    struct wonder_storage_key overlap = key(&pool_a, 0x100000, 0x1000, 32, 0x3f);
    wonder_storage_insert(&overlap, (void *)3, release);
    assert(released[1] == 1 && !wonder_storage_find(&a) && wonder_storage_find(&overlap) == (void *)3 && wonder_storage_count() == 2);
    struct wonder_storage_key after = key(&pool_a, 0x101000, 0x1000, 32, 0x3f);          // adjacent, not overlapping
    wonder_storage_insert(&after, (void *)4, release);
    assert(released[3] == 0 && wonder_storage_count() == 3);
    wonder_storage_forget_pool(&pool_a);
    assert(released[3] == 1 && released[4] == 1 && released[2] == 0 && wonder_storage_count() == 1);
    assert(wonder_storage_find(&other_pool) == (void *)2);
    puts("texture_storage OK");
    return 0;
}
