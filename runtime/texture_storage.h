#pragma once
// Host GPU textures by storage: (memory pool, offset, size, layout). Texture objects are views of pool memory on
// the console, so a texture re-created over the same memory with the same layout sees the same contents. Not
// thread-safe: the caller holds its lock.
#include "nvn_tex.h"
#include <stdint.h>
struct wonder_storage_key { const void *pool; int64_t offset; uint64_t size; nvn_texdesc desc; };
void *wonder_storage_find(const struct wonder_storage_key *key);                 // exact match, or NULL
// Inserts after releasing every entry whose memory overlaps (that memory now holds this texture).
void wonder_storage_insert(const struct wonder_storage_key *key, void *object, void (*release)(void *));
void wonder_storage_forget_pool(const void *pool);                                // nvnMemoryPoolFinalize
unsigned wonder_storage_count(void);
