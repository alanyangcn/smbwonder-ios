#pragma once
// NVN textures for Wonder: objects are copied by value by the game, so host resources are keyed by the
// initialization serial, never by object address (same rule as D2R dm/gfx/nvn).
#include "nvn_device.h"
#include "nvn_tex.h"
#define WONDER_NVN_TEXTURE_BUILDER_SIZE 0x80   // builders at sp+0x58 with a 128-byte stride (main+0x984a38)
#define WONDER_NVN_TEXTURE_SIZE 0x80           // main+0x984bd0 allocator request (header says 0xC0; the game allocates 0x80)
#define WONDER_NVN_TEXTURE_VIEW_SIZE 0x10 // sdk+0x45bee0; main+0x42cd5c view ends at region sp+0x18
struct wonder_nvn_texture {
    uint32_t magic, flags; nvn_texdesc desc; const void *pool; int64_t offset; uint64_t size;
    void *host;            // display textures: retained host render target; others: created at first use
    uint64_t serial; uint8_t swizzle[4], depth_stencil_mode, has_layout, layout[8];
};
struct wonder_nvn_texture_view {
    // SDK 55 layout (sdk+0x45bee0..0x45c060), also safe for native SDK getters/copies.
    int16_t min_layer, layers;
    uint8_t base_level, levels, target;
    uint8_t set_levels:1, set_layers:1, set_format:1, set_swizzle:1,
            set_depth_stencil:1, set_target:1, depth_stencil_mode:2;
    int32_t format;
    uint8_t swizzle[4];
};
// Snapshot taken at TexturePoolRegisterTexture: NVN copies the descriptor into pool memory at that point.
struct wonder_nvn_texture_registration { uint64_t serial; struct wonder_nvn_texture texture; struct wonder_nvn_texture_view view; uint8_t has_view, valid; };
const struct wonder_nvn_texture *wonder_nvn_texture_of(const void *texture, const char *who);
const struct wonder_nvn_texture_registration *wonder_nvn_texture_lookup(const void *pool, int id);
size_t wonder_nvn_texture_registration_size(void);
