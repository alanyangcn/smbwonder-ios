// NVN texture builder, textures, views and texture-pool registration (main+0x77714 helper, 0x984a38 display path).
// Storage sizes/alignments follow the Tegra X1 block-linear layout (dm/gfx/nvn/nvn_tex.c, deko3d-derived) because
// game data (BNTX) and pool offsets are computed from them. Host Metal textures for non-display textures are
// created at first use by the render layer (METAL2-RENDER-PLAN.md section 4), not here.
#include "nvn_texture.h"
#include "nvn_fmt_mtl.h"
#include "nvn_deswizzle.h"
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#define TBUILDER_MAGIC 0x42585457u
#define DEVICE_SET_MARK 0x56454454u      /* SetDevice may precede SetDefaults (main+0x984a4c, 0x77748) */
#define TEXTURE_MAGIC 0x54584554u        /* "TEXT" */
struct texture_builder { uint32_t magic, device_mark; const void *device; nvn_texdesc desc; const void *pool; int64_t offset;
                         uint8_t swizzle[4], depth_stencil_mode, has_layout, layout[8]; const void *packaged_data; };
_Static_assert(sizeof(struct texture_builder) <= WONDER_NVN_TEXTURE_BUILDER_SIZE, "texture builder storage");
_Static_assert(sizeof(struct wonder_nvn_texture) <= WONDER_NVN_TEXTURE_SIZE, "texture storage");
_Static_assert(sizeof(struct wonder_nvn_texture_view) <= WONDER_NVN_TEXTURE_VIEW_SIZE, "texture view storage");
static _Atomic uint64_t next_serial = 1;

static struct texture_builder *tbuilder_of(void *p, const char *who) {
    struct texture_builder *b = p;
    if (!b || b->magic != TBUILDER_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT %s texture_builder=%p (SetDefaults not called)", who, p);
    return b;
}
void wonder_nvnTextureBuilderSetDevice(void *builder, const void *device) {
    if (!builder) wonder_nvn_stop("NVN_INVALID_OBJECT nvnTextureBuilderSetDevice builder=NULL");
    struct texture_builder *b = builder;
    b->device = wonder_nvn_device_check(device, "nvnTextureBuilderSetDevice"); b->device_mark = DEVICE_SET_MARK;
}
void wonder_nvnTextureBuilderSetDefaults(void *builder) {
    if (!builder) wonder_nvn_stop("NVN_INVALID_OBJECT nvnTextureBuilderSetDefaults builder=NULL");
    struct texture_builder *b = builder;
    const void *device = b->device_mark == DEVICE_SET_MARK ? b->device : 0;   // NVN: SetDefaults keeps the device
    memset(b, 0, sizeof *b);
    b->magic = TBUILDER_MAGIC; b->device = device; b->device_mark = device ? DEVICE_SET_MARK : 0;
    b->desc.target = 1; b->desc.width = b->desc.height = b->desc.depth = b->desc.levels = 1;   // TEXTURE_2D, 1x1x1, one level
    b->swizzle[0] = 2; b->swizzle[1] = 3; b->swizzle[2] = 4; b->swizzle[3] = 5;                  // identity R G B A
}
static int dimension(int v, const char *who) { if (v <= 0 || v > 16384) wonder_nvn_stop("NVN_INVALID_ARGUMENT %s %d", who, v); return v; }
void wonder_nvnTextureBuilderSetSize2D(void *builder, int w, int h) {
    struct texture_builder *b = tbuilder_of(builder, "nvnTextureBuilderSetSize2D");
    b->desc.width = (uint32_t)dimension(w, "nvnTextureBuilderSetSize2D width"); b->desc.height = (uint32_t)dimension(h, "nvnTextureBuilderSetSize2D height"); b->desc.depth = 1;
}
void wonder_nvnTextureBuilderSetWidth(void *builder, int v) { tbuilder_of(builder, "nvnTextureBuilderSetWidth")->desc.width = (uint32_t)dimension(v, "nvnTextureBuilderSetWidth"); }
void wonder_nvnTextureBuilderSetHeight(void *builder, int v) { tbuilder_of(builder, "nvnTextureBuilderSetHeight")->desc.height = (uint32_t)dimension(v, "nvnTextureBuilderSetHeight"); }
void wonder_nvnTextureBuilderSetDepth(void *builder, int v) { tbuilder_of(builder, "nvnTextureBuilderSetDepth")->desc.depth = (uint32_t)dimension(v, "nvnTextureBuilderSetDepth"); }
void wonder_nvnTextureBuilderSetLevels(void *builder, int v) {
    if (v <= 0 || v > 15) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureBuilderSetLevels %d", v);
    tbuilder_of(builder, "nvnTextureBuilderSetLevels")->desc.levels = (uint32_t)v;
}
void wonder_nvnTextureBuilderSetTarget(void *builder, int target) {
    struct texture_builder *b = tbuilder_of(builder, "nvnTextureBuilderSetTarget");
    if (target < 0 || target > 10) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureBuilderSetTarget target=%d", target);
    b->desc.target = target;
}
void wonder_nvnTextureBuilderSetFormat(void *builder, int format) {
    struct texture_builder *b = tbuilder_of(builder, "nvnTextureBuilderSetFormat");
    if (!nvn_fmt_bpb(format, 0, 0)) wonder_nvn_stop("NVN_UNSUPPORTED nvnTextureBuilderSetFormat format=%#x (no layout data)", (unsigned)format);
    b->desc.format = format;
}
void wonder_nvnTextureBuilderSetFlags(void *builder, int flags) { tbuilder_of(builder, "nvnTextureBuilderSetFlags")->desc.flags = flags; }
int wonder_nvnTextureBuilderGetFlags(const void *builder) { return tbuilder_of((void *)builder, "nvnTextureBuilderGetFlags")->desc.flags; }
void wonder_nvnTextureBuilderSetSamples(void *builder, int samples) {
    if (samples != 0 && samples != 1 && samples != 2 && samples != 4 && samples != 8) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureBuilderSetSamples %d", samples);
    tbuilder_of(builder, "nvnTextureBuilderSetSamples")->desc.samples = samples;
}
static uint8_t swz(int v, const char *who) { if (v < 0 || v > 5) wonder_nvn_stop("NVN_INVALID_ARGUMENT %s swizzle=%d", who, v); return (uint8_t)v; }
void wonder_nvnTextureBuilderSetSwizzle(void *builder, int r, int g, int b_, int a) {
    struct texture_builder *b = tbuilder_of(builder, "nvnTextureBuilderSetSwizzle");
    b->swizzle[0] = swz(r, "nvnTextureBuilderSetSwizzle"); b->swizzle[1] = swz(g, "nvnTextureBuilderSetSwizzle");
    b->swizzle[2] = swz(b_, "nvnTextureBuilderSetSwizzle"); b->swizzle[3] = swz(a, "nvnTextureBuilderSetSwizzle");
}
void wonder_nvnTextureBuilderSetDepthStencilMode(void *builder, int mode) {
    if (mode != 0 && mode != 1) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureBuilderSetDepthStencilMode %d", mode);
    tbuilder_of(builder, "nvnTextureBuilderSetDepthStencilMode")->depth_stencil_mode = (uint8_t)mode;
}
/* NVNpackagedTextureLayout is the 8-byte nn::gfx TextureInfo+0x24 blob (main+0x1084ef4 tail-calls this when info flag bit0
 * is set). Survey of all 3063 BRTI entries in update 262144 RomFS: u32 block-height log2 (0..4) followed by u32 0x00010007.
 * Byte 0 selects the level-0 GOB block height (dm/gfx/nvn/nvn_tex.c pkg_block_h); the second word has only ever been
 * observed with that one value, so anything else stops rather than being guessed. NULL clears the packaged layout. */
#define PACKAGED_LAYOUT_WORD1 0x00010007u
void wonder_nvnTextureBuilderSetPackagedTextureLayout(void *builder, const void *layout) {
    struct texture_builder *b = tbuilder_of(builder, "nvnTextureBuilderSetPackagedTextureLayout");
    if (!layout) { b->has_layout = 0; memset(b->layout, 0, sizeof b->layout); b->desc.pkg_block_h = 0; return; }
    uint8_t raw[8]; uint32_t w0, w1; memcpy(raw, layout, 8); memcpy(&w0, raw, 4); memcpy(&w1, raw + 4, 4);
    if (w0 > 5 || w1 != PACKAGED_LAYOUT_WORD1)
        wonder_nvn_stop("NVN_UNSUPPORTED nvnTextureBuilderSetPackagedTextureLayout layout=%02x%02x%02x%02x%02x%02x%02x%02x "
                        "(expected u32 block-height log2 <= 5, u32 %#x)", raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6],
                        raw[7], PACKAGED_LAYOUT_WORD1);
    b->has_layout = 1; memcpy(b->layout, raw, 8); b->desc.pkg_block_h = (int)w0 + 1;
}
void wonder_nvnTextureBuilderSetStride(void *builder, ptrdiff_t stride) {
    if (stride < 0 || stride > 0x7fffffff) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureBuilderSetStride %td", stride);
    tbuilder_of(builder, "nvnTextureBuilderSetStride")->desc.stride = (uint32_t)stride;
}
static nvn_texlayout layout_of(const nvn_texdesc *d, const char *who) {
    nvn_texlayout L; nvn_tex_layout(d, &L);
    if (!L.bpb || !L.size) wonder_nvn_stop("NVN_UNSUPPORTED %s format=%#x target=%d (no TX1 layout)", who, (unsigned)d->format, d->target);
    return L;
}
size_t wonder_nvnTextureBuilderGetStorageSize(const void *builder) {
    const struct texture_builder *b = tbuilder_of((void *)builder, "nvnTextureBuilderGetStorageSize");
    nvn_texlayout L = layout_of(&b->desc, "nvnTextureBuilderGetStorageSize");
    wonder_nvn_report("NVN_CALL nvnTextureBuilderGetStorageSize %ux%ux%u levels=%u target=%d format=%#x flags=%#x -> %#llx (TX1 block height 2^%u GOB)",
                      b->desc.width, b->desc.height, b->desc.depth, b->desc.levels, b->desc.target, (unsigned)b->desc.format,
                      (unsigned)b->desc.flags, (unsigned long long)L.size, L.tileH);
    return (size_t)L.size;
}
// ZCull save/restore storage (sdk+0x45d0d0 computes it from TX1 ZCull hardware parameters and returns 0 for formats
// without ZCull). The host GPU has no saveable ZCull (Metal manages its own hierarchical depth), so no storage is needed;
// nvnCommandBufferSave/RestoreZCullData remain stops if a caller ever uses them. Round72: main+0x1e1f918 stores the
// result in its depth-buffer object.
size_t wonder_nvnTextureBuilderGetZCullStorageSize(const void *builder) {
    tbuilder_of((void *)builder, "nvnTextureBuilderGetZCullStorageSize");
    return 0;
}
size_t wonder_nvnTextureBuilderGetStorageAlignment(const void *builder) {
    return layout_of(&tbuilder_of((void *)builder, "nvnTextureBuilderGetStorageAlignment")->desc, "nvnTextureBuilderGetStorageAlignment").align;
}
void wonder_nvnTextureBuilderSetStorage(void *builder, const void *pool, ptrdiff_t offset) {
    struct texture_builder *b = tbuilder_of(builder, "nvnTextureBuilderSetStorage");
    struct wonder_nvn_pool_view view; wonder_nvn_pool_check(pool, "nvnTextureBuilderSetStorage", &view);
    if (offset < 0 || (uint64_t)offset >= view.size) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureBuilderSetStorage offset=%#tx pool=%#llx", offset, (unsigned long long)view.size);
    b->pool = pool; b->offset = offset;
}

// Storage class (TX1 page kind) groups formats that may share virtual-pool pages. Host-defined but stable:
// it is only passed back through NVNmappingRequest (main+0x42ee64 builds a per-format table for MapVirtual).
static int storage_class_of(const nvn_texdesc *d) {
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    if (!bpb) wonder_nvn_stop("NVN_UNSUPPORTED storage class for format=%#x", (unsigned)d->format);
    int depth = d->format >= 0x31 && d->format <= 0x36, compressed = bw > 1 || bh > 1;
    return 0x100 | (depth ? 0x80 : 0) | (compressed ? 0x40 : 0) | (d->samples > 1 ? 0x20 : 0) | (bpb & 0x1f);
}
int wonder_nvnTextureBuilderGetStorageClass(const void *builder) {
    return storage_class_of(&tbuilder_of((void *)builder, "nvnTextureBuilderGetStorageClass")->desc);
}

// ---------- Textures ----------
// Diagnostic (round126: text never drawn): the first R8 pitch-linear texture is the font glyph cache (2048x8192);
// wonder_nvn_glyph_cache_nonzero samples its storage so the log tells whether glyphs are rasterized at all.
static const uint8_t *glyph_cache; static uint64_t glyph_cache_size, glyph_cache_serial;
size_t wonder_nvn_glyph_cache_nonzero(uint64_t *serial) {
    size_t n = 0; if (serial) *serial = glyph_cache_serial;
    uint64_t first = UINT64_MAX, last = 0;
    for (uint64_t i = 0; glyph_cache && i < glyph_cache_size; i += 64) if (glyph_cache[i]) { n++; if (first == UINT64_MAX) first = i; last = i; }
    if (n) fprintf(stderr, "HOST_GLYPH_CACHE rows %llu..%llu of %llu (2048-byte rows)\n", (unsigned long long)(first / 2048), (unsigned long long)(last / 2048), (unsigned long long)(glyph_cache_size / 2048));
    return n;
}
static int display_format(int format) { return format == 0x25 || format == 0x38 || format == 0x9A || format == 0x9C; }  // RGBA8(_SRGB), BGRA8(_SRGB)
uint32_t wonder_nvnTextureInitialize(void *texture, const void *builder) {
    const struct texture_builder *b = tbuilder_of((void *)builder, "nvnTextureInitialize");
    if (!texture) wonder_nvn_stop("NVN_INVALID_OBJECT nvnTextureInitialize texture=NULL");
    if (!b->device) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureInitialize: builder has no device");
    if (!b->pool) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureInitialize: no storage");
    nvn_texlayout L = layout_of(&b->desc, "nvnTextureInitialize");
    struct wonder_nvn_pool_view view; wonder_nvn_pool_check(b->pool, "nvnTextureInitialize", &view);
    if ((uint64_t)b->offset % L.align || (uint64_t)b->offset + L.size > view.size)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureInitialize offset=%#llx size=%#llx align=%u pool=%#llx",
                        (unsigned long long)b->offset, (unsigned long long)L.size, L.align, (unsigned long long)view.size);
    uint32_t mtl = 0; int mapping = nvn_fmt_to_mtl(b->desc.format, &mtl, NULL);   // BC formats are sampled natively (Metal 3 GPUs)
    void *host = NULL;
    if (b->desc.flags & 1) {   // DISPLAY: host render target that the window presents
        if (b->desc.target != 1 || b->desc.levels != 1 || !display_format(b->desc.format))
            wonder_nvn_stop("NVN_UNSUPPORTED nvnTextureInitialize display texture target=%d levels=%u format=%#x", b->desc.target, b->desc.levels, (unsigned)b->desc.format);
        host = wonder_host_display_texture(b->desc.width, b->desc.height, b->desc.format);
        if (!host) wonder_nvn_stop("NVN_HOST_TEXTURE_FAILED nvnTextureInitialize %ux%u format=%#x", b->desc.width, b->desc.height, (unsigned)b->desc.format);
    }
    if (b->desc.format == 0x1 && (b->desc.flags & 0x10) && !(view.flags & 0x200)) {   // diagnostic: font glyph cache storage
        glyph_cache = view.memory + b->offset; glyph_cache_size = L.size; glyph_cache_serial = atomic_load(&next_serial);
    }
    struct wonder_nvn_texture *t = texture;
    memset(t, 0, sizeof *t);
    *t = (struct wonder_nvn_texture){TEXTURE_MAGIC, (uint32_t)b->desc.flags, b->desc, b->pool, b->offset, L.size, host,
                                     atomic_fetch_add(&next_serial, 1), {b->swizzle[0], b->swizzle[1], b->swizzle[2], b->swizzle[3]},
                                     b->depth_stencil_mode, b->has_layout, {0}};
    memcpy(t->layout, b->layout, sizeof t->layout);
    wonder_nvn_report("NVN_TEXTURE_READY serial=%llu %ux%ux%u levels=%u target=%d format=%#x flags=%#x samples=%d pool_offset=%#llx size=%#llx metal=%u(%s)%s",
                      (unsigned long long)t->serial, b->desc.width, b->desc.height, b->desc.depth, b->desc.levels, b->desc.target,
                      (unsigned)b->desc.format, (unsigned)b->desc.flags, b->desc.samples, (unsigned long long)b->offset, (unsigned long long)L.size,
                      mtl, mapping == 1 ? "exact" : mapping == 2 ? "substitute" : "expand/none",
                      host ? " host=display" : "");
    return 1;
}
const struct wonder_nvn_texture *wonder_nvn_texture_of(const void *p, const char *who) {
    const struct wonder_nvn_texture *t = p;
    if (!t || t->magic != TEXTURE_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT %s texture=%p", who, p);
    return t;
}
void wonder_nvnTextureFinalize(void *texture) {
    struct wonder_nvn_texture *t = (struct wonder_nvn_texture *)wonder_nvn_texture_of(texture, "nvnTextureFinalize");
    if (t->host) wonder_nvn_stop("NVN_UNSUPPORTED nvnTextureFinalize on a display texture (window teardown not implemented)");
    t->magic = 0;   // registrations keep their own snapshot; the render layer releases host objects by serial
    wonder_host_texture_forget(t->serial);
}
void wonder_nvnTextureSetDebugLabel(void *texture, const char *label) {
    const struct wonder_nvn_texture *t=wonder_nvn_texture_of(texture,"nvnTextureSetDebugLabel");
    if(t->host) wonder_host_resource_label(t->host,label);
    wonder_nvn_report("NVN_TEXTURE_LABEL object=%p label=%s",texture,label?label:"");
}
int wonder_nvnTextureGetFlags(const void *t) { return (int)wonder_nvn_texture_of(t, "nvnTextureGetFlags")->flags; }
int wonder_nvnTextureGetFormat(const void *t) { return wonder_nvn_texture_of(t, "nvnTextureGetFormat")->desc.format; }
int wonder_nvnTextureGetWidth(const void *t) { return (int)wonder_nvn_texture_of(t, "nvnTextureGetWidth")->desc.width; }
int wonder_nvnTextureGetHeight(const void *t) { return (int)wonder_nvn_texture_of(t, "nvnTextureGetHeight")->desc.height; }
int wonder_nvnTextureGetDepth(const void *t) { return (int)wonder_nvn_texture_of(t, "nvnTextureGetDepth")->desc.depth; }
int wonder_nvnTextureGetLevels(const void *t) { return (int)wonder_nvn_texture_of(t, "nvnTextureGetLevels")->desc.levels; }
int wonder_nvnTextureGetTarget(const void *t) { return wonder_nvn_texture_of(t, "nvnTextureGetTarget")->desc.target; }

int wonder_nvnTextureGetStorageClass(const void *t) { return storage_class_of(&wonder_nvn_texture_of(t, "nvnTextureGetStorageClass")->desc); }

// CPU texel upload (main+0x42cdc0): the game's pool storage stays the single source of truth, in TX1
// block-linear layout; the render layer uploads from it at first use. NVNcopyRegion = x,y,z offsets + w,h,d.
struct copy_region { int x, y, z, w, h, d; };
void wonder_nvnTextureWriteTexels(const void *texture, const void *view, const void *region, const void *data) {
    const struct wonder_nvn_texture *t = wonder_nvn_texture_of(texture, "nvnTextureWriteTexels");
    const struct wonder_nvn_texture_view *v = view;
    const struct copy_region *r = region;
    if (!r || !data || r->x < 0 || r->y < 0 || r->z < 0 || r->w <= 0 || r->h <= 0 || r->d <= 0)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureWriteTexels region/data");
    struct wonder_nvn_pool_view pool; wonder_nvn_pool_check(t->pool, "nvnTextureWriteTexels", &pool);
    if (pool.flags & 1) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureWriteTexels on CPU_NO_ACCESS storage");
    unsigned level = v && v->set_levels ? (unsigned)v->base_level : 0, layer0 = v && v->set_layers ? (unsigned)v->min_layer : 0;
    // Validate texel coordinates before converting to byte/block coordinates. A large x multiplied
    // by bpb in uint32_t can otherwise wrap to zero and silently overwrite the first texel.
    if (level >= t->desc.levels || level >= 32)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureWriteTexels mip=%u", level);
    uint32_t width = t->desc.width >> level, height = t->desc.height >> level;
    if (!width) width = 1;
    if (!height) height = 1;
    if ((uint32_t)r->x > width || (uint32_t)r->w > width - (uint32_t)r->x ||
        (uint32_t)r->y > height || (uint32_t)r->h > height - (uint32_t)r->y ||
        (uint64_t)r->z + (uint64_t)r->d > UINT32_MAX)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureWriteTexels region outside mip");
    int bw, bh, bpb = nvn_fmt_bpb(t->desc.format, &bw, &bh);
    if (r->x % bw || r->y % bh) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureWriteTexels region not block aligned");
    int is3d = t->desc.target == 2;
    uint32_t row = (uint32_t)((r->w + bw - 1) / bw * bpb), rows = (uint32_t)((r->h + bh - 1) / bh);
    const uint8_t *src = data;
    for (int z = 0; z < r->d; z++) {
        uint32_t row_bytes, lrows, slices, lbpb; uint64_t base; unsigned th, td;
        unsigned layer = is3d ? layer0 : layer0 + ((unsigned)r->z + (unsigned)z);
        if (nvn_deswizzle_geom(&t->desc, level, layer, &row_bytes, &lrows, &slices, &base, &th, &td, &lbpb))
            wonder_nvn_stop("NVN_UNSUPPORTED nvnTextureWriteTexels target=%d flags=%#x samples=%d level=%u layer=%u",
                            t->desc.target, t->flags, t->desc.samples, level, layer);
        uint32_t gx0 = (uint32_t)(r->x / bw) * lbpb, gy0 = (uint32_t)(r->y / bh), gz = is3d ? ((uint32_t)r->z + (uint32_t)z) : 0;
        if (gx0 + row > row_bytes || gy0 + rows > lrows || gz >= slices)
            wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureWriteTexels region outside level %u", level);
        if (t->offset < 0 || (uint64_t)t->offset > pool.size || base >= pool.size - (uint64_t)t->offset) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureWriteTexels storage");
        for (uint32_t y = 0; y < rows; y++)
            for (uint32_t x = 0; x < row; ) {   // runs never cross a 16-byte GOB segment
                uint32_t gx = gx0 + x, n = 16 - gx % 16; if (n > row - x) n = row - x;
                uint64_t swizzled = nvn_bl_offset(gx, gy0 + y, gz, row_bytes, lrows, th, td);
                uint64_t available = pool.size - (uint64_t)t->offset - base;
                if (swizzled > available || n > available - swizzled)
                    wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureWriteTexels destination outside pool");
                memcpy(pool.memory + t->offset + base + swizzled, src + (size_t)y * row + x, n);
                x += n;
            }
        src += (size_t)row * rows;
    }
}
// Pool memory is coherent with the GPU on this host; the render layer re-uploads from storage at use.
void wonder_nvnTextureFlushTexels(const void *texture, const void *view, const void *region) {
    (void)view; (void)region; wonder_nvn_texture_of(texture, "nvnTextureFlushTexels");
}

// ---------- Texture views (value objects) ----------
void wonder_nvnTextureViewSetDefaults(void *v) { if (!v) wonder_nvn_stop("NVN_INVALID_OBJECT nvnTextureViewSetDefaults view=NULL"); memset(v, 0, sizeof(struct wonder_nvn_texture_view)); }
void wonder_nvnTextureViewSetLevels(void *v, int base, int n) {
    if (base < 0 || base > 15 || n < 0 || n > 15 - base) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureViewSetLevels base=%d n=%d", base, n);
    struct wonder_nvn_texture_view *o = v; o->set_levels = 1; o->base_level = base; o->levels = n;
}
void wonder_nvnTextureViewSetLayers(void *v, int min, int n) {
    if (min < 0 || min > 32767 || n < 0 || n > 32767) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureViewSetLayers min=%d n=%d", min, n);
    struct wonder_nvn_texture_view *o = v; o->set_layers = 1; o->min_layer = min; o->layers = n;
}
void wonder_nvnTextureViewSetFormat(void *v, int format) {
    if (!nvn_fmt_bpb(format, 0, 0)) wonder_nvn_stop("NVN_UNSUPPORTED nvnTextureViewSetFormat format=%#x", (unsigned)format);
    struct wonder_nvn_texture_view *o = v; o->set_format = 1; o->format = format;
}
void wonder_nvnTextureViewSetTarget(void *v, int target) {
    if (target < 0 || target > 10) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureViewSetTarget %d", target);
    struct wonder_nvn_texture_view *o = v; o->set_target = 1; o->target = target;
}
void wonder_nvnTextureViewSetSwizzle(void *v, int r, int g, int b, int a) {
    struct wonder_nvn_texture_view *o = v; o->set_swizzle = 1;
    o->swizzle[0] = swz(r, "nvnTextureViewSetSwizzle"); o->swizzle[1] = swz(g, "nvnTextureViewSetSwizzle");
    o->swizzle[2] = swz(b, "nvnTextureViewSetSwizzle"); o->swizzle[3] = swz(a, "nvnTextureViewSetSwizzle");
}
void wonder_nvnTextureViewSetDepthStencilMode(void *v, int mode) {
    if (mode != 0 && mode != 1) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTextureViewSetDepthStencilMode %d", mode);
    struct wonder_nvn_texture_view *o = v; o->set_depth_stencil = 1; o->depth_stencil_mode = (uint8_t)mode;
}

// ---------- Texture pool registration: snapshot of descriptor + view ----------
size_t wonder_nvn_texture_registration_size(void) { return sizeof(struct wonder_nvn_texture_registration); }
struct descriptor_pool_header { uint32_t magic; int32_t count; const void *memory; int64_t offset; void *host; };   // nvn_device.c layout
#define TPOOL_MAGIC 0x4c505854u
static int registrations_logged;
void wonder_nvnTexturePoolRegisterTexture(const void *pool, int id, const void *texture, const void *view) {
    const struct descriptor_pool_header *p = pool;
    if (!p || p->magic != TPOOL_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT nvnTexturePoolRegisterTexture pool=%p", pool);
    if (id < 0 || id >= p->count) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnTexturePoolRegisterTexture id=%d count=%d", id, p->count);
    const struct wonder_nvn_texture *t = wonder_nvn_texture_of(texture, "nvnTexturePoolRegisterTexture");
    struct wonder_nvn_texture_registration *r = &((struct wonder_nvn_texture_registration *)p->host)[id];
    memset(r, 0, sizeof *r);
    r->serial = t->serial; r->texture = *t; r->has_view = view != NULL; r->valid = 1;
    if (view) r->view = *(const struct wonder_nvn_texture_view *)view;
    if (registrations_logged++ < 32)
        wonder_nvn_report("NVN_CALL nvnTexturePoolRegisterTexture id=%d serial=%llu format=%#x %ux%u view=%d", id,
                          (unsigned long long)t->serial, (unsigned)t->desc.format, t->desc.width, t->desc.height, view != NULL);
}
const struct wonder_nvn_texture_registration *wonder_nvn_texture_lookup(const void *pool, int id) {
    const struct descriptor_pool_header *p = pool;
    if (!p || p->magic != TPOOL_MAGIC || id < 0 || id >= p->count) return 0;
    const struct wonder_nvn_texture_registration *r = &((const struct wonder_nvn_texture_registration *)p->host)[id];
    return r->valid ? r : 0;
}

// View getters (sdk+0x45bfa0/0x45bfc0/0x45c050): outputs plus whether the field was set.
static const struct wonder_nvn_texture_view *view_in(const void *v, const char *who) {
    if (!v) wonder_nvn_stop("NVN_INVALID_OBJECT %s view=NULL", who);
    return v;
}
uint32_t wonder_nvnTextureViewGetLevels(const void *v, int *base, int *count) {
    const struct wonder_nvn_texture_view *o = view_in(v, "nvnTextureViewGetLevels"); *base = o->base_level; *count = o->levels; return o->set_levels;
}
uint32_t wonder_nvnTextureViewGetLayers(const void *v, int *min, int *count) {
    const struct wonder_nvn_texture_view *o = view_in(v, "nvnTextureViewGetLayers"); *min = o->min_layer; *count = (uint16_t)o->layers; return o->set_layers;
}
uint32_t wonder_nvnTextureViewGetTarget(const void *v, int *target) {
    const struct wonder_nvn_texture_view *o = view_in(v, "nvnTextureViewGetTarget"); *target = o->target; return o->set_target;
}
// sdk+0x45be10 only stores the pointer (builder+0x60); Initialize never reads it in this driver.
void wonder_nvnTextureBuilderSetPackagedTextureData(void *builder, const void *data) {
    tbuilder_of(builder, "nvnTextureBuilderSetPackagedTextureData")->packaged_data = data;
}
const struct wonder_nvn_entry wonder_nvn_texture_entries[] = {
#define E(n) {#n, (void *)wonder_##n}
    E(nvnTextureViewGetLevels), E(nvnTextureViewGetLayers), E(nvnTextureViewGetTarget), E(nvnTextureBuilderSetPackagedTextureData),
    E(nvnTextureBuilderSetDevice), E(nvnTextureBuilderSetDefaults), E(nvnTextureBuilderSetSize2D), E(nvnTextureBuilderSetWidth),
    E(nvnTextureBuilderSetHeight), E(nvnTextureBuilderSetDepth), E(nvnTextureBuilderSetLevels), E(nvnTextureBuilderSetTarget),
    E(nvnTextureBuilderSetFormat), E(nvnTextureBuilderSetFlags), E(nvnTextureBuilderGetFlags), E(nvnTextureBuilderSetSamples),
    E(nvnTextureBuilderSetSwizzle), E(nvnTextureBuilderSetPackagedTextureLayout), E(nvnTextureBuilderSetDepthStencilMode), E(nvnTextureBuilderSetStride),
    E(nvnTextureBuilderGetStorageSize), E(nvnTextureBuilderGetZCullStorageSize), E(nvnTextureBuilderGetStorageAlignment), E(nvnTextureBuilderSetStorage),
    E(nvnTextureInitialize), E(nvnTextureFinalize), E(nvnTextureSetDebugLabel), E(nvnTextureGetFlags), E(nvnTextureGetFormat),
    E(nvnTextureGetWidth), E(nvnTextureGetHeight), E(nvnTextureGetDepth), E(nvnTextureGetLevels), E(nvnTextureGetTarget),
    E(nvnTextureViewSetDefaults), E(nvnTextureViewSetLevels), E(nvnTextureViewSetLayers), E(nvnTextureViewSetFormat),
    E(nvnTextureViewSetTarget), E(nvnTextureViewSetSwizzle), E(nvnTextureViewSetDepthStencilMode), E(nvnTexturePoolRegisterTexture),
    E(nvnTextureBuilderGetStorageClass), E(nvnTextureGetStorageClass), E(nvnTextureWriteTexels), E(nvnTextureFlushTexels),
#undef E
};
const unsigned wonder_nvn_texture_count = sizeof wonder_nvn_texture_entries / sizeof *wonder_nvn_texture_entries;
