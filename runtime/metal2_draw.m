#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
#include <stdio.h>
#include "metal2_draw.h"
#include "host_draw.h"
#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
static id<MTLDevice> device;
static NSString *directory; static NSDictionary *manifest;
static NSMutableDictionary<NSString *, id<MTLFunction>> *functions;
static NSMutableDictionary<NSString *, NSValue *> *reflections;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void wonder_host_draw_set_device(void *mtl_device) { device = (__bridge id<MTLDevice>)mtl_device; }
void wonder_host_shader_set_directory(const char *path) {
    pthread_mutex_lock(&lock);
    directory = [NSString stringWithUTF8String:path]; manifest = nil; functions = [NSMutableDictionary dictionary]; reflections = [NSMutableDictionary dictionary];
    pthread_mutex_unlock(&lock);
}
static NSString *sha256(NSData *data) {
    unsigned char h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(data.bytes, (CC_LONG)data.length, h);
    NSMutableString *s = [NSMutableString stringWithCapacity:64]; for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [s appendFormat:@"%02x", h[i]];
    return s;
}
// Verified bytes of <key>.<suffix>: the manifest entry must exist, its stage must match, and the file must hash to it.
static NSData *verified(NSString *key, NSString *suffix, NSString *hash_field, int stage) {
    if (!directory) wonder_nvn_stop("NVN_SHADER_MISSING key=%s: no shader directory", key.UTF8String);
    if (!manifest) {
        NSData *raw = [NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:@"manifest.json"]];
        manifest = raw ? [NSJSONSerialization JSONObjectWithData:raw options:0 error:nil] : nil;
        if (![manifest[@"keys"] isKindOfClass:NSDictionary.class]) wonder_nvn_stop("NVN_SHADER_MISSING key=%s: no manifest.json in %s", key.UTF8String, directory.UTF8String);
    }
    NSDictionary *entry = manifest[@"keys"][key];
    if (!entry) wonder_nvn_stop("NVN_SHADER_MISSING key=%s (not in the shader build; run tools/wonder_shader_build.py on its draw census)", key.UTF8String);
    if (stage >= 0 && [entry[@"stage"] isEqual:@"ps"] != (stage == 1))
        wonder_nvn_stop("NVN_SHADER_STAGE key=%s built as %s, bound as stage %d", key.UTF8String, [entry[@"stage"] UTF8String], stage);
    NSData *data = [NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:[key stringByAppendingPathExtension:suffix]]];
    if (!data) wonder_nvn_stop("NVN_SHADER_MISSING key=%s: no %s file", key.UTF8String, suffix.UTF8String);
    if (![sha256(data) isEqualToString:entry[hash_field]]) wonder_nvn_stop("NVN_SHADER_MANIFEST key=%s: %s does not match manifest.json", key.UTF8String, suffix.UTF8String);
    return data;
}
const struct wonder_shader_refl *wonder_host_shader_refl(const char key_c[17]) {
    pthread_mutex_lock(&lock);
    NSString *key = [NSString stringWithUTF8String:key_c];
    NSValue *cached = reflections[key];
    if (!cached) {
        NSData *text = verified(key, @"refl", @"refl_sha256", -1);
        struct wonder_shader_refl *r = malloc(sizeof *r); char err[160];
        if (!r || wonder_shader_refl_parse(text.bytes, text.length, r, err, sizeof err)) wonder_nvn_stop("NVN_SHADER_REFLECTION key=%s: %s", key_c, r ? err : "out of memory");
        cached = [NSValue valueWithPointer:r]; reflections[key] = cached;   // lives for the process
    }
    pthread_mutex_unlock(&lock);
    return cached.pointerValue;
}
void *wonder_host_shader_function(const char key_c[17], int stage) {
    pthread_mutex_lock(&lock);
    NSString *key = [NSString stringWithUTF8String:key_c];
    id<MTLFunction> f = functions[key];
    if (!f) {
        NSData *source = verified(key, @"metal", @"msl_sha256", stage);
        if (!device) wonder_nvn_stop("NVN_SHADER_COMPILE_FAILED key=%s: no Metal device", key_c);
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_1; options.fastMathEnabled = NO;   // keep the translator's precision contract
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:[[NSString alloc] initWithData:source encoding:NSUTF8StringEncoding] options:options error:&error];
        if (!library) {
            NSString *first = [error.localizedDescription componentsSeparatedByString:@"\n"].firstObject ?: @"?";
            wonder_nvn_stop("NVN_SHADER_COMPILE_FAILED key=%s error=%s", key_c, first.UTF8String);
        }
        f = [library newFunctionWithName:@"main0"];
        MTLFunctionType want = stage ? MTLFunctionTypeFragment : MTLFunctionTypeVertex;
        if (!f || f.functionType != want) wonder_nvn_stop("NVN_SHADER_STAGE key=%s: main0 is not a %s function", key_c, stage ? "fragment" : "vertex");
        f.label = key; functions[key] = f;
    } else if (f.functionType != (stage ? MTLFunctionTypeFragment : MTLFunctionTypeVertex))
        wonder_nvn_stop("NVN_SHADER_STAGE key=%s bound as stage %d", key_c, stage);
    pthread_mutex_unlock(&lock);
    return (__bridge void *)f;
}
int wonder_host_shader_available(const char key_c[17]) {
    pthread_mutex_lock(&lock);
    if (!manifest && directory) {
        NSData *raw = [NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:@"manifest.json"]];
        manifest = raw ? [NSJSONSerialization JSONObjectWithData:raw options:0 error:nil] : nil;
    }
    int available = [manifest[@"keys"] isKindOfClass:NSDictionary.class] && manifest[@"keys"][[NSString stringWithUTF8String:key_c]] != nil;
    pthread_mutex_unlock(&lock);
    return available;
}
// ---------- Draw encoding ----------
// One render pass per draw (load/store), appended to the replay's pending command buffer. Every NVN state the first
// draws use is mapped; anything else stops by name instead of being approximated.
#include "nvn_fmt_mtl.h"
#include "nvn_bcdec.h"
#include "nvn_deswizzle.h"
_Noreturn static void unsupported(const struct wonder_host_draw *d, const char *what, long value) {
    wonder_nvn_stop("NVN_DRAW_UNSUPPORTED %s %ld (vs=%s fs=%s)", what, value, d->vs_key, d->fs_key);
}
uint32_t wonder_host_vertex_format(int f) {
    switch (f) {
    case 0x0A: return MTLVertexFormatFloat;   case 0x16: return MTLVertexFormatFloat2;  case 0x22: return MTLVertexFormatFloat3;  case 0x2E: return MTLVertexFormatFloat4;   // R/RG/RGB/RGBA32F
    case 0x05: return MTLVertexFormatHalf;    case 0x11: return MTLVertexFormatHalf2;   case 0x1D: return MTLVertexFormatHalf3;   case 0x29: return MTLVertexFormatHalf4;    // R/RG/RGB/RGBA16F
    case 0x01: return MTLVertexFormatUCharNormalized; case 0x0D: return MTLVertexFormatUChar2Normalized; case 0x19: return MTLVertexFormatUChar3Normalized; case 0x25: return MTLVertexFormatUChar4Normalized;   // R/RG/RGB/RGBA8
    case 0x02: return MTLVertexFormatCharNormalized;  case 0x0E: return MTLVertexFormatChar2Normalized;  case 0x1A: return MTLVertexFormatChar3Normalized;  case 0x26: return MTLVertexFormatChar4Normalized;    // ..8SN
    case 0x03: return MTLVertexFormatUChar;   case 0x0F: return MTLVertexFormatUChar2;  case 0x1B: return MTLVertexFormatUChar3;  case 0x27: return MTLVertexFormatUChar4;   // ..8UI
    case 0x04: return MTLVertexFormatChar;    case 0x10: return MTLVertexFormatChar2;   case 0x1C: return MTLVertexFormatChar3;   case 0x28: return MTLVertexFormatChar4;    // ..8I
    case 0x06: return MTLVertexFormatUShortNormalized; case 0x12: return MTLVertexFormatUShort2Normalized; case 0x1E: return MTLVertexFormatUShort3Normalized; case 0x2A: return MTLVertexFormatUShort4Normalized; // ..16
    case 0x07: return MTLVertexFormatShortNormalized;  case 0x13: return MTLVertexFormatShort2Normalized;  case 0x1F: return MTLVertexFormatShort3Normalized;  case 0x2B: return MTLVertexFormatShort4Normalized;  // ..16SN
    case 0x08: return MTLVertexFormatUShort;  case 0x14: return MTLVertexFormatUShort2; case 0x20: return MTLVertexFormatUShort3; case 0x2C: return MTLVertexFormatUShort4;  // ..16UI
    case 0x09: return MTLVertexFormatShort;   case 0x15: return MTLVertexFormatShort2;  case 0x21: return MTLVertexFormatShort3;  case 0x2D: return MTLVertexFormatShort4;   // ..16I
    case 0x0B: return MTLVertexFormatUInt;    case 0x17: return MTLVertexFormatUInt2;   case 0x23: return MTLVertexFormatUInt3;   case 0x2F: return MTLVertexFormatUInt4;    // ..32UI
    case 0x0C: return MTLVertexFormatInt;     case 0x18: return MTLVertexFormatInt2;    case 0x24: return MTLVertexFormatInt3;    case 0x30: return MTLVertexFormatInt4;     // ..32I
    default: return MTLVertexFormatInvalid;
    }
}
static MTLBlendFactor blend_factor(const struct wonder_host_draw *d, unsigned f) {
    switch (f) {
    case 1: return MTLBlendFactorZero; case 2: return MTLBlendFactorOne; case 3: return MTLBlendFactorSourceColor; case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha; case 6: return MTLBlendFactorOneMinusSourceAlpha; case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha; case 9: return MTLBlendFactorDestinationColor; case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated; case 0x10: return MTLBlendFactorSource1Color; case 0x11: return MTLBlendFactorOneMinusSource1Color;
    case 0x12: return MTLBlendFactorSource1Alpha; case 0x13: return MTLBlendFactorOneMinusSource1Alpha;
    default: unsupported(d, "blend factor (constant blend colour is not tracked yet)", (long)f);
    }
}
static MTLBlendOperation blend_op(const struct wonder_host_draw *d, unsigned e) {
    switch (e) { case 1: return MTLBlendOperationAdd; case 2: return MTLBlendOperationSubtract; case 3: return MTLBlendOperationReverseSubtract;
                 case 4: return MTLBlendOperationMin; case 5: return MTLBlendOperationMax; default: unsupported(d, "blend equation", (long)e); }
}
static MTLCompareFunction compare(const struct wonder_host_draw *d, unsigned f) {   // NVN depth/compare funcs 1..8 = NEVER..ALWAYS
    if (f < 1 || f > 8) unsupported(d, "compare function", (long)f);
    static const MTLCompareFunction map[9] = {0, MTLCompareFunctionNever, MTLCompareFunctionLess, MTLCompareFunctionEqual, MTLCompareFunctionLessEqual,
                                             MTLCompareFunctionGreater, MTLCompareFunctionNotEqual, MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways};
    return map[f];
}
static MTLPixelFormat attachment_format(const struct wonder_host_draw *d, const struct wonder_nvn_texture *t, int depth) {
    uint32_t mtl = 0; int fit = nvn_fmt_to_mtl(t->desc.format, &mtl, NULL);
    if (!(fit == 1 || (fit == 2 && depth)) || !mtl) unsupported(d, depth ? "depth target format" : "colour target format", t->desc.format);
    return (MTLPixelFormat)mtl;
}
static id<MTLTexture> attachment(const struct wonder_host_draw *d, const struct wonder_nvn_texture_snapshot *s, int depth, int *level, int *slice) {
    if (s->texture.desc.target != 1 && s->texture.desc.target != 4 && s->texture.desc.target != 7) unsupported(d, "render target texture target", s->texture.desc.target);
    *level = s->has_view && s->view.set_levels ? s->view.base_level : 0; *slice = s->has_view && s->view.set_layers ? s->view.min_layer : 0;
    id<MTLTexture> t = (__bridge id<MTLTexture>)wonder_host_render_target(&s->texture, (uint32_t)attachment_format(d, &s->texture, depth));
    if (!t) unsupported(d, "render target creation failed for serial", (long)s->texture.serial);
    return t;
}
static uint64_t fnv64(const uint8_t *p, uint64_t n) { uint64_t h = 1469598103934665603ull; for (uint64_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull; return h; }
static NSMutableDictionary<NSNumber *, id<MTLTexture>> *sampled; static NSMutableDictionary<NSNumber *, NSNumber *> *sampled_hash;
static id<MTLTexture> sampled_texture(const struct wonder_host_draw *d, int stage, int slot) {
    const struct wonder_nvn_texture *t = &d->tex[stage][slot].texture.texture;
    const struct wonder_nvn_texture_registration *reg = &d->tex[stage][slot].texture;
    if (reg->has_view && (reg->view.set_format || reg->view.set_target || reg->view.set_swizzle || reg->view.set_depth_stencil ||
                          (reg->view.set_levels && reg->view.base_level) || (reg->view.set_layers && reg->view.min_layer)))
        unsupported(d, "texture view on sampled texture serial", (long)t->serial);
    id<MTLTexture> gpu = (__bridge id<MTLTexture>)wonder_host_render_target_existing(t);
    id<MTLTexture> tex = gpu;
    if (!tex) {
        if (t->desc.target != 1) unsupported(d, "sampled texture target (2D only so far)", t->desc.target);
        if (!d->tex[stage][slot].storage) unsupported(d, "sampled texture without CPU storage, serial", (long)t->serial);
        uint32_t mtl = 0; int fit = nvn_fmt_to_mtl(t->desc.format, &mtl, NULL), bc_bpp = 0; uint32_t bc_pf = 0;
        int bc = fit != 1 && nvn_bc_unpacked((uint32_t)t->desc.format, &bc_pf, &bc_bpp);
        if (fit != 1 && !bc) unsupported(d, "sampled texture format", t->desc.format);
        if (bc && ![device supportsFamily:MTLGPUFamilyApple1]) unsupported(d, "BC decode device", 0);
        uint64_t hash = fnv64(d->tex[stage][slot].storage, d->tex[stage][slot].storage_size);
        NSNumber *key = @(t->serial);
        if (!sampled) { sampled = [NSMutableDictionary dictionary]; sampled_hash = [NSMutableDictionary dictionary]; }
        tex = sampled[key];
        if (!tex || sampled_hash[key].unsignedLongLongValue != hash) {
            MTLPixelFormat pf = (MTLPixelFormat)(bc ? bc_pf : mtl);
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:t->desc.width height:t->desc.height mipmapped:t->desc.levels > 1];
            td.mipmapLevelCount = t->desc.levels ? t->desc.levels : 1; td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView; td.storageMode = MTLStorageModeShared;
            tex = [device newTextureWithDescriptor:td];
            if (!tex) unsupported(d, "sampled texture allocation, serial", (long)t->serial);
            for (unsigned level = 0; level < td.mipmapLevelCount; level++) {
                uint32_t row_bytes, rows, slices, lbpb; uint64_t base; unsigned th, tdp;
                if (nvn_deswizzle_geom(&t->desc, level, 0, &row_bytes, &rows, &slices, &base, &th, &tdp, &lbpb)) unsupported(d, "texture layout level", level);
                if (base + (uint64_t)row_bytes * rows > d->tex[stage][slot].storage_size) unsupported(d, "texture level beyond storage, level", level);
                NSMutableData *linear = [NSMutableData dataWithLength:(NSUInteger)row_bytes * rows];
                if (nvn_deswizzle(&t->desc, level, 0, d->tex[stage][slot].storage, linear.mutableBytes, row_bytes)) unsupported(d, "texture deswizzle level", level);
                uint32_t w = t->desc.width >> level ? t->desc.width >> level : 1, h = t->desc.height >> level ? t->desc.height >> level : 1;
                if (bc) {
                    NSMutableData *rgba = [NSMutableData dataWithLength:(NSUInteger)w * h * (NSUInteger)bc_bpp];
                    if (nvn_bc_decode((uint32_t)t->desc.format, linear.bytes, row_bytes, w, h, rgba.mutableBytes, (size_t)w * (size_t)bc_bpp)) unsupported(d, "BC decode level", level);
                    [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level withBytes:rgba.bytes bytesPerRow:(NSUInteger)w * (NSUInteger)bc_bpp];
                } else [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level withBytes:linear.bytes bytesPerRow:row_bytes];
            }
            tex.label = [NSString stringWithFormat:@"NVN sampled %llu %ux%u fmt %#x", (unsigned long long)t->serial, t->desc.width, t->desc.height, t->desc.format];
            sampled[key] = tex; sampled_hash[key] = @(hash);
        }
    }
    const uint8_t *z = t->swizzle;   // NVN texture swizzle values equal MTLTextureSwizzle (ZERO 0, ONE 1, R 2, G 3, B 4, A 5)
    int identity = z[0] == 2 && z[1] == 3 && z[2] == 4 && z[3] == 5, unset = !(z[0] | z[1] | z[2] | z[3]);
    if (identity || unset) return tex;
    if (z[0] > 5 || z[1] > 5 || z[2] > 5 || z[3] > 5) unsupported(d, "texture swizzle", z[0] | z[1] << 8 | z[2] << 16 | z[3] << 24);
    return [tex newTextureViewWithPixelFormat:tex.pixelFormat textureType:tex.textureType levels:NSMakeRange(0, tex.mipmapLevelCount) slices:NSMakeRange(0, 1)
                                     swizzle:MTLTextureSwizzleChannelsMake(z[0], z[1], z[2], z[3])];
}
static NSMutableDictionary<NSData *, id<MTLSamplerState>> *samplers;
static id<MTLSamplerState> sampler_state(const struct wonder_host_draw *d, const struct wonder_nvn_sampler_state *m) {
    NSData *key = [NSData dataWithBytes:m length:sizeof *m];
    if (!samplers) samplers = [NSMutableDictionary dictionary];
    id<MTLSamplerState> s = samplers[key]; if (s) return s;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    if (m->min > 5 || m->mag > 1) unsupported(d, "sampler filter", m->min | m->mag << 8);
    sd.minFilter = (m->min & 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;   // NEAREST 0, LINEAR 1, *_MIPMAP_* 2..5
    sd.mipFilter = m->min < 2 ? MTLSamplerMipFilterNotMipmapped : m->min < 4 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    sd.magFilter = m->mag ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    const uint8_t wraps[3] = {m->wrap_s, m->wrap_t, m->wrap_r}; MTLSamplerAddressMode modes[3];
    for (int i = 0; i < 3; i++) switch (wraps[i]) {
        case 0: case 7: modes[i] = MTLSamplerAddressModeClampToEdge; break;   // CLAMP (legacy GL clamp) treated as clamp-to-edge
        case 1: modes[i] = MTLSamplerAddressModeRepeat; break;
        case 2: case 3: modes[i] = MTLSamplerAddressModeMirrorClampToEdge; break;
        case 6: modes[i] = MTLSamplerAddressModeMirrorRepeat; break;
        default: unsupported(d, "sampler wrap mode (border colours are not mapped yet)", wraps[i]);
    }
    sd.sAddressMode = modes[0]; sd.tAddressMode = modes[1]; sd.rAddressMode = modes[2];
    sd.lodMinClamp = m->lod_min; sd.lodMaxClamp = m->lod_max;
    if (m->lod_bias != 0) unsupported(d, "sampler LOD bias x1000", (long)(m->lod_bias * 1000));
    if (m->reduction) unsupported(d, "sampler reduction filter", m->reduction);
    sd.maxAnisotropy = m->anisotropy >= 1 ? (NSUInteger)m->anisotropy : 1;
    if (m->compare_mode) sd.compareFunction = compare(d, m->compare_func);
    s = [device newSamplerStateWithDescriptor:sd];
    if (!s) unsupported(d, "sampler creation", 0);
    samplers[key] = s; return s;
}
static NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *pipelines;
static NSMutableDictionary<NSData *, id<MTLDepthStencilState>> *depth_states;
static id<MTLBuffer> support_buffer;
int wonder_host_draw(const struct wonder_host_draw *d) {
    if (!device) wonder_nvn_stop("NVN_DRAW_UNSUPPORTED no Metal device (vs=%s fs=%s)", d->vs_key, d->fs_key);
    // Fixed-function checks that need shader-side emulation are refused, not approximated.
    if ((d->states_set & 1) && d->color.logic_op != 3) unsupported(d, "colour logic op", d->color.logic_op);
    if ((d->states_set & 1) && d->color.alpha_func != 8) unsupported(d, "alpha test function", d->color.alpha_func);
    if ((d->states_set & 16) && (d->multisample.enable || d->multisample.samples > 1 || d->multisample.a2c)) unsupported(d, "multisample state samples", d->multisample.samples);
    if ((d->states_set & 4) && d->depth_stencil.st) unsupported(d, "stencil test (reference/masks not tracked yet)", 1);
    if ((d->states_set & 8) && d->polygon.mode != 2) unsupported(d, "polygon mode", d->polygon.mode);
    MTLPrimitiveType primitive;
    switch (d->primitive) { case 0: primitive = MTLPrimitiveTypePoint; break; case 1: primitive = MTLPrimitiveTypeLine; break; case 3: primitive = MTLPrimitiveTypeLineStrip; break;
                            case 4: primitive = MTLPrimitiveTypeTriangle; break; case 5: primitive = MTLPrimitiveTypeTriangleStrip; break; default: unsupported(d, "draw primitive", d->primitive); }
    if (d->instances < 1) return 0;
    // Attachments.
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = (__bridge id<MTLFunction>)d->vs; pd.fragmentFunction = (__bridge id<MTLFunction>)d->fs;
    NSUInteger width = 0, height = 0;
    for (int i = 0; i < d->color_count && i < 8; i++) {
        if (!d->targets[i].valid) continue;
        int level, slice; id<MTLTexture> t = attachment(d, &d->targets[i], 0, &level, &slice);
        pass.colorAttachments[i].texture = t; pass.colorAttachments[i].level = level; pass.colorAttachments[i].slice = slice;
        pass.colorAttachments[i].loadAction = MTLLoadActionLoad; pass.colorAttachments[i].storeAction = MTLStoreActionStore;
        MTLRenderPipelineColorAttachmentDescriptor *c = pd.colorAttachments[i]; c.pixelFormat = t.pixelFormat;
        unsigned m = (d->states_set & 2) ? d->channel_mask.mask >> (4 * i) & 15 : 15;   // NVN R1 G2 B4 A8
        c.writeMask = (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) | (m & 4 ? MTLColorWriteMaskBlue : 0) | (m & 8 ? MTLColorWriteMaskAlpha : 0);
        if ((d->states_set & 1) && (d->color.blend_en >> i & 1)) {
            if (!(d->blend_set >> i & 1)) unsupported(d, "blend enabled without a blend state for target", i);
            const wonder_nvn_blend_state *b = &d->blend[i];
            c.blendingEnabled = YES; c.sourceRGBBlendFactor = blend_factor(d, b->src_rgb); c.destinationRGBBlendFactor = blend_factor(d, b->dst_rgb);
            c.sourceAlphaBlendFactor = blend_factor(d, b->src_a); c.destinationAlphaBlendFactor = blend_factor(d, b->dst_a);
            c.rgbBlendOperation = blend_op(d, b->eq_rgb); c.alphaBlendOperation = blend_op(d, b->eq_a);
        }
        if (!width) { width = MAX((NSUInteger)1, t.width >> level); height = MAX((NSUInteger)1, t.height >> level); }
    }
    const struct wonder_nvn_texture_snapshot *ds = &d->targets[d->color_count < 8 ? d->color_count : 8];
    if (ds->valid) {
        int level, slice; id<MTLTexture> t = attachment(d, ds, 1, &level, &slice);
        MTLPixelFormat f = t.pixelFormat;
        if (f != MTLPixelFormatStencil8) { pass.depthAttachment.texture = t; pass.depthAttachment.level = level; pass.depthAttachment.slice = slice;
            pass.depthAttachment.loadAction = MTLLoadActionLoad; pass.depthAttachment.storeAction = MTLStoreActionStore; pd.depthAttachmentPixelFormat = f; }
        if (f == MTLPixelFormatStencil8 || f == MTLPixelFormatDepth32Float_Stencil8) { pass.stencilAttachment.texture = t; pass.stencilAttachment.level = level; pass.stencilAttachment.slice = slice;
            pass.stencilAttachment.loadAction = MTLLoadActionLoad; pass.stencilAttachment.storeAction = MTLStoreActionStore; pd.stencilAttachmentPixelFormat = f; }
        if (!width) { width = MAX((NSUInteger)1, t.width >> level); height = MAX((NSUInteger)1, t.height >> level); }
    }
    if (!width) unsupported(d, "draw without render targets", 0);
    // Vertex layout: attribute locations from the vertex reflection, streams at [[buffer(16 + stream)]].
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (int i = 0; i < d->nattr; i++) {
        uint32_t f = wonder_host_vertex_format((int)d->attr[i].format);
        if (!f) unsupported(d, "vertex format", (long)d->attr[i].format);
        MTLVertexAttributeDescriptor *a = vd.attributes[d->vr->attr[i].location];
        a.format = (MTLVertexFormat)f; a.offset = d->attr[i].offset; a.bufferIndex = WONDER_SHADER_STREAM_BUFFER_BASE + d->attr[i].stream;
    }
    for (int s = 0; s < 16; s++) if (d->stream_mask >> s & 1) {
        MTLVertexBufferLayoutDescriptor *l = vd.layouts[WONDER_SHADER_STREAM_BUFFER_BASE + s];
        if (!d->stream[s].stride) { l.stepFunction = MTLVertexStepFunctionConstant; l.stepRate = 0; l.stride = 16; }
        else { l.stride = d->stream[s].stride; l.stepFunction = d->stream[s].divisor ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex; l.stepRate = d->stream[s].divisor ? d->stream[s].divisor : 1; }
    }
    pd.vertexDescriptor = vd;
    // Pipeline cache keyed by everything the descriptor depends on.
    struct { void *vs, *fs; uint32_t formats[10]; uint64_t blend[8]; uint32_t color, mask; uint32_t attr[16][3]; uint32_t stream[16][2]; } pk;
    memset(&pk, 0, sizeof pk); pk.vs = d->vs; pk.fs = d->fs;
    for (int i = 0; i < 8; i++) { pk.formats[i] = (uint32_t)pd.colorAttachments[i].pixelFormat; memcpy(&pk.blend[i], &d->blend[i], sizeof d->blend[i]); }
    pk.formats[8] = (uint32_t)pd.depthAttachmentPixelFormat; pk.formats[9] = (uint32_t)pd.stencilAttachmentPixelFormat;
    memcpy(&pk.color, &d->color, 4); memcpy(&pk.mask, &d->channel_mask, 4);
    for (int i = 0; i < d->nattr; i++) { pk.attr[i][0] = d->attr[i].format; pk.attr[i][1] = d->attr[i].offset; pk.attr[i][2] = d->attr[i].stream | (uint32_t)d->vr->attr[i].location << 8; }
    for (int s = 0; s < 16; s++) if (d->stream_mask >> s & 1) { pk.stream[s][0] = d->stream[s].stride; pk.stream[s][1] = d->stream[s].divisor; }
    NSData *pipeline_key = [NSData dataWithBytes:&pk length:sizeof pk];
    if (!pipelines) { pipelines = [NSMutableDictionary dictionary]; depth_states = [NSMutableDictionary dictionary]; }
    id<MTLRenderPipelineState> pso = pipelines[pipeline_key];
    if (!pso) {
        NSError *error = nil; pso = [device newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!pso) wonder_nvn_stop("NVN_DRAW_PIPELINE_FAILED vs=%s fs=%s: %s", d->vs_key, d->fs_key, error.localizedDescription.UTF8String);
        pipelines[pipeline_key] = pso;
    }
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    if ((d->states_set & 4) && d->depth_stencil.dt && pd.depthAttachmentPixelFormat) { dd.depthCompareFunction = compare(d, d->depth_stencil.df); dd.depthWriteEnabled = d->depth_stencil.dw; }
    NSData *ds_key = [NSData dataWithBytes:(uint32_t[]){(uint32_t)dd.depthCompareFunction, dd.depthWriteEnabled} length:8];
    id<MTLDepthStencilState> dss = depth_states[ds_key];
    if (!dss) { dss = [device newDepthStencilStateWithDescriptor:dd]; depth_states[ds_key] = dss; }
    // Resolve every resource before taking the replay lock: texture lookups and render-target creation take the same
    // (non-recursive) lock in host_graphics.m (round55 deadlocked at the first draw).
    const struct wonder_shader_refl *refl[2] = {d->vr, d->fr};
    id<MTLTexture> textures[2][32]; id<MTLSamplerState> sampler_states[2][32]; id<MTLBuffer> c1_buffers[2] = {nil, nil};
    for (int stage = 0; stage < 2; stage++) {
        const struct wonder_shader_refl *r = refl[stage];
        for (int i = 0; i < r->ntex; i++) {
            int slot = r->tex[i].slot;
            textures[stage][i] = sampled_texture(d, stage, slot); sampler_states[stage][i] = sampler_state(d, &d->tex[stage][slot].sampler);
        }
        if (r->c1_buffer >= 0) {   // the shader's own constants; padded to the 64 KiB a constant buffer may declare
            c1_buffers[stage] = [device newBufferWithLength:0x10000 options:MTLResourceStorageModeShared]; memcpy(c1_buffers[stage].contents, r->c1, r->c1_len);
        }
        if (r->support >= 0 && !support_buffer) {   // Ryujinx support buffer (layout per D2R nvn_mtl.m): render_scale[0..72].x = 1, everything else 0
            support_buffer = [device newBufferWithLength:0x10000 options:MTLResourceStorageModeShared];
            for (int k = 0; k < 73; k++) *(float *)((uint8_t *)support_buffer.contents + 192 + 16 * k) = 1.0f;
        }
    }
    // Encode.
    wonder_host_replay_lock();
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)wonder_host_replay_command_buffer();
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
    if (!e) { wonder_host_replay_unlock(); unsupported(d, "render encoder creation", 0); }
    e.label = [NSString stringWithFormat:@"NVN draw vs=%s fs=%s", d->vs_key, d->fs_key];
    [e setRenderPipelineState:pso]; [e setDepthStencilState:dss];
    MTLCullMode cull = MTLCullModeNone;
    if (d->states_set & 8) { if (d->polygon.cull == 3) { wonder_host_replay_unlock(); unsupported(d, "cull FRONT_AND_BACK", 3); }
                             cull = d->polygon.cull == 1 ? MTLCullModeFront : d->polygon.cull == 2 ? MTLCullModeBack : MTLCullModeNone; }
    [e setCullMode:cull];
    // NVN front face (CW 0 / CCW 1) is defined on clip-space winding; with UPPER_LEFT origin Metal's framebuffer winding
    // matches the GL clip-control convention, so the NVN value maps directly (pinned by the back-face GPU test).
    [e setFrontFacingWinding:(d->states_set & 8) && d->polygon.front == 0 ? MTLWindingClockwise : MTLWindingCounterClockwise];
    MTLViewport vp = {0, 0, (double)width, (double)height, d->depth_range[0], d->depth_range[1]};
    if (d->viewport_set) { vp.originX = d->viewport[0]; vp.originY = d->viewport[1]; vp.width = d->viewport[2]; vp.height = d->viewport[3]; }
    [e setViewport:vp];
    if (d->scissor_set) {
        long x = MAX(0, d->scissor[0]), y = MAX(0, d->scissor[1]);
        long x1 = MIN((long)width, (long)d->scissor[0] + d->scissor[2]), y1 = MIN((long)height, (long)d->scissor[1] + d->scissor[3]);
        if (x1 <= x || y1 <= y) { [e endEncoding]; wonder_host_replay_unlock(); return 0; }   // empty scissor: nothing is drawn
        [e setScissorRect:(MTLScissorRect){(NSUInteger)x, (NSUInteger)y, (NSUInteger)(x1 - x), (NSUInteger)(y1 - y)}];
    }
    for (int stage = 0; stage < 2; stage++) {
        const struct wonder_shader_refl *r = refl[stage];
        void (^buffer)(id<MTLBuffer>, NSUInteger, NSUInteger) = ^(id<MTLBuffer> b, NSUInteger offset, NSUInteger index) {
            if (stage) [e setFragmentBuffer:b offset:offset atIndex:index]; else [e setVertexBuffer:b offset:offset atIndex:index]; };
        for (int i = 0; i < r->ncbuf; i++) { const struct wonder_host_span *u = &d->ubo[stage][r->cbuf[i].slot]; buffer((__bridge id<MTLBuffer>)u->buffer, u->offset, r->cbuf[i].buffer); }
        if (r->c1_buffer >= 0) buffer(c1_buffers[stage], 0, (NSUInteger)r->c1_buffer);
        if (r->support >= 0) buffer(support_buffer, 0, (NSUInteger)r->support);
        for (int i = 0; i < r->ntex; i++) {
            if (stage) { [e setFragmentTexture:textures[stage][i] atIndex:r->tex[i].texture]; [e setFragmentSamplerState:sampler_states[stage][i] atIndex:r->tex[i].sampler]; }
            else { [e setVertexTexture:textures[stage][i] atIndex:r->tex[i].texture]; [e setVertexSamplerState:sampler_states[stage][i] atIndex:r->tex[i].sampler]; }
        }
    }
    for (int s = 0; s < 16; s++) if (d->stream_mask >> s & 1)
        [e setVertexBuffer:(__bridge id<MTLBuffer>)d->stream[s].span.buffer offset:d->stream[s].span.offset atIndex:WONDER_SHADER_STREAM_BUFFER_BASE + s];
    if (d->index_type < 0) {
        [e drawPrimitives:primitive vertexStart:(NSUInteger)d->first vertexCount:(NSUInteger)d->count instanceCount:(NSUInteger)d->instances baseInstance:(NSUInteger)d->base_instance];
    } else {
        id<MTLBuffer> ib = (__bridge id<MTLBuffer>)d->index.buffer; NSUInteger offset = d->index.offset; MTLIndexType type = MTLIndexTypeUInt16;
        if (d->index_type == 0) {   // Metal has no 8-bit indices: widen to 16 bits for this draw
            id<MTLBuffer> wide = [device newBufferWithLength:MAX((NSUInteger)2, (NSUInteger)d->count * 2) options:MTLResourceStorageModeShared];
            const uint8_t *src = (const uint8_t *)ib.contents + offset; uint16_t *dst = wide.contents;
            for (int i = 0; i < d->count; i++) dst[i] = src[i];
            ib = wide; offset = 0;
        } else if (d->index_type == 2) type = MTLIndexTypeUInt32;
        [e drawIndexedPrimitives:primitive indexCount:(NSUInteger)d->count indexType:type indexBuffer:ib indexBufferOffset:offset
                   instanceCount:(NSUInteger)d->instances baseVertex:d->base_vertex baseInstance:(NSUInteger)d->base_instance];
    }
    [e endEncoding];
    wonder_host_replay_unlock();
    return 0;
}

// ---------- Render target readback (evidence) ----------
int wonder_host_readback_png(void *texture, int level, int slice, const char *path_c) {
    id<MTLTexture> t = (__bridge id<MTLTexture>)texture;
    if (!t || !device || !path_c) return 1;
    MTLPixelFormat f = t.pixelFormat;
    int bgra = f == MTLPixelFormatBGRA8Unorm || f == MTLPixelFormatBGRA8Unorm_sRGB;
    if (!bgra && f != MTLPixelFormatRGBA8Unorm && f != MTLPixelFormatRGBA8Unorm_sRGB) return 2;
    NSUInteger w = MAX((NSUInteger)1, t.width >> level), h = MAX((NSUInteger)1, t.height >> level), pitch = w * 4;
    id<MTLBuffer> out = [device newBufferWithLength:pitch * h options:MTLResourceStorageModeShared];
    NSString *path = [NSString stringWithUTF8String:path_c];
    wonder_host_replay_lock();
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)wonder_host_replay_command_buffer();
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:out destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:pitch * h];
    [b endEncoding];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        if (done.status != MTLCommandBufferStatusCompleted) { fprintf(stderr, "HOST_READBACK_FAILED %s status=%ld\n", path.UTF8String, (long)done.status); return; }
        uint8_t *p = out.contents;
        if (bgra) for (NSUInteger i = 0; i < w * h; i++) { uint8_t x = p[4 * i]; p[4 * i] = p[4 * i + 2]; p[4 * i + 2] = x; }
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, p, pitch * h, NULL);
        CGImageRef image = CGImageCreate(w, h, 8, 32, pitch, space, kCGImageAlphaLast | kCGBitmapByteOrderDefault, provider, NULL, false, kCGRenderingIntentDefault);
        CGImageDestinationRef destination = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], CFSTR("public.png"), 1, NULL);
        int ok = 0; if (image && destination) { CGImageDestinationAddImage(destination, image, NULL); ok = CGImageDestinationFinalize(destination); }
        if (destination) CFRelease(destination); if (image) CGImageRelease(image); CGDataProviderRelease(provider); CGColorSpaceRelease(space);
        (void)out;   // keeps the buffer alive until the PNG is written
        fprintf(stderr, "HOST_READBACK %s %lux%lu ok=%d\n", path.UTF8String, (unsigned long)w, (unsigned long)h, ok);
    }];
    wonder_host_replay_unlock();
    return 0;
}
