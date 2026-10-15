#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
#include <mach/mach.h>
#include <stdio.h>
#include <unistd.h>
#include <stdatomic.h>
#include <TargetConditionals.h>
#include "metal2_draw.h"
#include "host_draw.h"
#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
static id<MTLDevice> device;
// Metal compiles shaders and pipelines on the calling thread's stack on iOS; guest threads have small stacks (round84:
// ___chkstk_darwin overflow in the compiler). Such work runs on a GCD worker (normal stack) while the caller waits;
// dispatch_async + semaphore, because dispatch_sync may run the block on the caller's own stack.
static void on_host_stack(void (^work)(void)) {
    if (pthread_get_stacksize_np(pthread_self()) >= (1u << 20)) { work(); return; }
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ work(); dispatch_semaphore_signal(done); });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
}
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
        __block NSError *error = nil; __block id<MTLLibrary> library = nil;
        on_host_stack(^{ NSError *e = nil; library = [device newLibraryWithSource:[[NSString alloc] initWithData:source encoding:NSUTF8StringEncoding] options:options error:&e]; error = e; });
        if (!library) {
            NSString *first = nil;   // the compiler's own "program_source:L:C: error:" line when there is one
            for (NSString *line in [error.localizedDescription componentsSeparatedByString:@"\n"]) if ([line containsString:@"error:"]) { first = line; break; }
            if (!first) first = [error.localizedDescription componentsSeparatedByString:@"\n"].firstObject ?: @"?";
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
    case 0x61: return MTLBlendFactorBlendColor; case 0x62: return MTLBlendFactorOneMinusBlendColor;
    case 0x63: return MTLBlendFactorBlendAlpha; case 0x64: return MTLBlendFactorOneMinusBlendAlpha;
    default: unsupported(d, "blend factor", (long)f);
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
    int target = s->texture.desc.target;
    if (target != 1 && target != 2 && target != 4 && target != 7 && target != 8 && target != 9) unsupported(d, "render target texture target", target);
    *level = s->has_view && s->view.set_levels ? s->view.base_level : 0; *slice = s->has_view && s->view.set_layers ? s->view.min_layer : 0;
    id<MTLTexture> t = (__bridge id<MTLTexture>)wonder_host_render_target(&s->texture, (uint32_t)attachment_format(d, &s->texture, depth));
    if (!t) unsupported(d, "render target creation failed for serial", (long)s->texture.serial);
    return t;
}
// CPU-uploaded textures: shared on iOS; macOS (the GPU tests) does not allow shared textures, managed works with replaceRegion.
#if TARGET_OS_OSX
#define CPU_TEXTURE_STORAGE MTLStorageModeManaged
#else
#define CPU_TEXTURE_STORAGE MTLStorageModeShared
#endif
static int device_samples_bc(void) {
    const char *force = getenv("WONDER_TEST_NO_BC"); if (force && *force && strcmp(force, "0")) return 0;
#if TARGET_OS_OSX
    return 1;   // every Mac GPU samples BC
#else
    if (@available(iOS 16.4, *)) return device.supportsBCTextureCompression;
    return 0;
#endif
}
static uint64_t fnv64(const uint8_t *p, uint64_t n) { uint64_t h = 1469598103934665603ull; for (uint64_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull; return h; }
static NSMutableDictionary<NSNumber *, id<MTLTexture>> *sampled; static NSMutableDictionary<NSNumber *, NSNumber *> *sampled_hash;
static id<MTLTexture> sampled_texture(const struct wonder_host_draw *d, int stage, int slot) {
    const struct wonder_nvn_texture *t = &d->tex[stage][slot].texture.texture;
    const struct wonder_nvn_texture_registration *reg = &d->tex[stage][slot].texture;
    // Texture views (registered with the pool entry): same format and target as the texture, a level/layer sub-range and a
    // swizzle are executed as a Metal texture view; reinterpreting the format or target stops by name.
    const struct wonder_nvn_texture_view *v = reg->has_view ? &reg->view : NULL;
    if (v && v->set_format && v->format != t->desc.format) unsupported(d, "texture view format (reinterpretation) on serial", (long)t->serial);
    if (v && v->set_target && v->target != t->desc.target) unsupported(d, "texture view target on serial", (long)t->serial);
    if (v && v->set_depth_stencil && v->depth_stencil_mode && t->desc.format >= 0x31 && t->desc.format <= 0x36) unsupported(d, "stencil texturing view on serial", (long)t->serial);
    id<MTLTexture> gpu = (__bridge id<MTLTexture>)wonder_host_render_target_existing(t);
    id<MTLTexture> tex = gpu;
    if (!tex) {
        if (t->desc.target != 1) unsupported(d, "sampled texture target (2D only so far)", t->desc.target);
        if (!d->tex[stage][slot].storage) unsupported(d, "sampled texture without CPU storage, serial", (long)t->serial);
        uint32_t mtl = 0; int fit = nvn_fmt_to_mtl(t->desc.format, &mtl, NULL), bc_bpp = 0; uint32_t bc_pf = 0;
        // BC formats map "exactly" in the table, but the A11 cannot sample them: decode on the CPU unless the GPU has BC
        // (WONDER_TEST_NO_BC forces the decode on hosts that do, like D2R's DM_TEST_NO_BC).
        int bc = nvn_bc_unpacked((uint32_t)t->desc.format, &bc_pf, &bc_bpp) && !device_samples_bc();
        if (fit != 1 && !bc) unsupported(d, "sampled texture format", t->desc.format);
        if (bc) { static _Atomic int logged; if (atomic_fetch_add(&logged, 1) < 8) fprintf(stderr, "HOST_TEXTURE_BC_DECODE serial=%llu format=%#x %ux%u\n", (unsigned long long)t->serial, t->desc.format, t->desc.width, t->desc.height); }
        uint64_t hash = fnv64(d->tex[stage][slot].storage, d->tex[stage][slot].storage_size);
        NSNumber *key = @(t->serial);
        if (!sampled) { sampled = [NSMutableDictionary dictionary]; sampled_hash = [NSMutableDictionary dictionary]; }
        tex = sampled[key];
        if (!tex || sampled_hash[key].unsignedLongLongValue != hash) {
            MTLPixelFormat pf = (MTLPixelFormat)(bc ? bc_pf : mtl);
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:t->desc.width height:t->desc.height mipmapped:t->desc.levels > 1];
            td.mipmapLevelCount = t->desc.levels ? t->desc.levels : 1; td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView; td.storageMode = CPU_TEXTURE_STORAGE;
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
    NSUInteger levels = tex.mipmapLevelCount, base = 0, count = levels;
    NSRange all = wonder_host_view_slices(tex.textureType, tex.arrayLength, 0, 0), range = all;
    if (v && v->set_levels) { base = v->base_level; count = v->levels ? v->levels : levels - base; }
    if (v && v->set_layers) range = wonder_host_view_slices(tex.textureType, tex.arrayLength, (NSUInteger)MAX(0, v->min_layer), (NSUInteger)MAX(0, v->layers));
    if (base >= levels || count > levels - base) unsupported(d, "texture view levels beyond the texture, serial", (long)t->serial);
    if (!range.length) unsupported(d, "texture view layers not representable (cube views need whole cubes), serial", (long)t->serial);
    NSUInteger first = range.location, layers = range.length, slices = all.length;
    const uint8_t *z = v && v->set_swizzle ? v->swizzle : t->swizzle;   // NVN swizzle values equal MTLTextureSwizzle (ZERO 0, ONE 1, R 2, G 3, B 4, A 5)
    int identity = z[0] == 2 && z[1] == 3 && z[2] == 4 && z[3] == 5, unset = !(z[0] | z[1] | z[2] | z[3]);
    if (!identity && !unset && (z[0] > 5 || z[1] > 5 || z[2] > 5 || z[3] > 5)) unsupported(d, "texture swizzle", z[0] | z[1] << 8 | z[2] << 16 | z[3] << 24);
    int whole = base == 0 && count == levels && first == 0 && layers == slices;
    if (whole && (identity || unset)) return tex;
    id<MTLTexture> view = identity || unset   // the swizzle-taking call fails outright on GPUs without texture swizzle
        ? [tex newTextureViewWithPixelFormat:tex.pixelFormat textureType:tex.textureType levels:NSMakeRange(base, count) slices:NSMakeRange(first, layers)]
        : [tex newTextureViewWithPixelFormat:tex.pixelFormat textureType:tex.textureType levels:NSMakeRange(base, count) slices:NSMakeRange(first, layers)
                                     swizzle:MTLTextureSwizzleChannelsMake(z[0], z[1], z[2], z[3])];
    if (!view) unsupported(d, "texture view creation on serial", (long)t->serial);
    return view;
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
// Diagnostic mode (WONDER_DRAW_SYNC=1): commit and wait after every draw; a GPU error stops naming this draw and every
// buffer span it read (round62-64: a page fault inside one 11-draw submit).
static int bisect_dummy_textures;   // diagnostic bisect only
static id<MTLTexture> dummy_texture(void) {
    static id<MTLTexture> t; if (t) return t;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    td.storageMode = CPU_TEXTURE_STORAGE; t = [device newTextureWithDescriptor:td]; uint32_t px = 0xffffffff;
    [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&px bytesPerRow:4]; return t;
}
static struct wonder_host_span cpu_copy(struct wonder_host_span s) {   // bisect: same bytes in a fresh shared buffer
    id<MTLBuffer> src = (__bridge id<MTLBuffer>)s.buffer; if (!src || !s.size) return s;
    id<MTLBuffer> b = [device newBufferWithBytes:(uint8_t *)src.contents + s.offset length:s.size options:MTLResourceStorageModeShared];
    return (struct wonder_host_span){(__bridge_retained void *)b, 0, s.size};   // leaked on purpose: diagnostic run stops right after
}
// After a faulting draw: re-run it in separate command buffers with one resource class swapped each time.
static void bisect(const struct wonder_host_draw *d, char *out, size_t n) {
    static struct wonder_host_draw v; size_t at = 0; char e[256];
    for (int variant = 0; variant < 4; variant++) {
        v = *d;
        if (variant == 1) { if (v.index_type >= 0) v.index = cpu_copy(v.index); for (int s = 0; s < 16; s++) if (v.stream_mask >> s & 1) v.stream[s].span = cpu_copy(v.stream[s].span); }
        if (variant == 2) for (int st = 0; st < 2; st++) for (int r = 0; r < 14; r++) if (v.ubo[st][r].buffer) v.ubo[st][r] = cpu_copy(v.ubo[st][r]);
        bisect_dummy_textures = variant == 3;
        wonder_host_gpu_error_reset();
        unsetenv("WONDER_DRAW_SYNC"); wonder_host_draw(&v); setenv("WONDER_DRAW_SYNC", "1", 1);
        wonder_host_finish();
        int faulted = 0; for (int i = 0; i < 100 && !(faulted = wonder_host_gpu_error(e, sizeof e)); i++) usleep(1000);
        static const char *const names[4] = {"same", "vertex+index copied", "uniforms copied", "dummy textures"};
        at += (size_t)snprintf(out + at, n - at, " [%s: %s]", names[variant], faulted ? "FAULT" : "ok");
    }
    bisect_dummy_textures = 0;
}
static void page_protections(id<MTLBuffer> b, char *out, size_t n) {
    size_t at = 0; out[0] = 0;
    if (!b || b.storageMode != MTLStorageModeShared) { snprintf(out, n, "private"); return; }
    uintptr_t base = (uintptr_t)b.contents;
    for (NSUInteger o = 0; o < b.length && at + 2 < n; o += 0x4000) {
        vm_address_t address = base + o; vm_size_t size = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t object;
        kern_return_t kr = vm_region_64(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_64_t)&info, &count, &object);
        char c = kr != KERN_SUCCESS || address > base + o ? '?' : (info.protection & VM_PROT_WRITE) ? 'w' : (info.protection & VM_PROT_READ) ? 'r' : '-';
        out[at++] = c; out[at] = 0;
    }
}
static void sync_check(const struct wonder_host_draw *d) {
    wonder_host_finish();
    char error[256];
    for (int i = 0; i < 100 && !wonder_host_gpu_error(error, sizeof error); i++) usleep(1000);   // completion handlers run asynchronously
    if (!wonder_host_gpu_error(error, sizeof error)) return;
    char spans[1400]; size_t at = 0;
    // For each buffer: its guest address range and the protection of every 16 KiB host page under it right now
    // (r/w/- per page, '?' = no VM region): a zero-copy buffer whose pages were remapped faults on the GPU.
    #define SPAN(name, sp) do { id<MTLBuffer> b_ = (__bridge id<MTLBuffer>)(sp).buffer; char prot_[40]; page_protections(b_, prot_, sizeof prot_); \
        at += (size_t)snprintf(spans + at, sizeof spans - at, " %s=%p+%#zx/%#zx(len %#lx contents %p pages %s)", name, \
        (sp).buffer, (sp).offset, (sp).size, b_ ? (unsigned long)b_.length : 0ul, b_ ? b_.contents : NULL, prot_); } while (0)
    if (d->index_type >= 0) SPAN("index", d->index);
    for (int s = 0; s < 16 && at < sizeof spans - 100; s++) if (d->stream_mask >> s & 1) { char n[16]; snprintf(n, sizeof n, "stream%d", s); SPAN(n, d->stream[s].span); }
    const struct wonder_shader_refl *refl[2] = {d->vr, d->fr};
    for (int st = 0; st < 2; st++) for (int i = 0; i < refl[st]->ncbuf && at < sizeof spans - 100; i++) {
        char n[16]; snprintf(n, sizeof n, "ubo%d.%d", st, refl[st]->cbuf[i].slot); SPAN(n, d->ubo[st][refl[st]->cbuf[i].slot]); }
    for (int st = 0; st < 2; st++) for (int r = 0; r < 32 && at < sizeof spans - 60; r++) if (d->tex_mask[st] >> r & 1)
        at += (size_t)snprintf(spans + at, sizeof spans - at, " tex%d.%d=serial %llu", st, r, (unsigned long long)d->tex[st][r].texture.texture.serial);
    #undef SPAN
    char variants[256]; bisect(d, variants, sizeof variants);
    wonder_nvn_stop("NVN_DRAW_GPU_FAULT vs=%s fs=%s primitive=%d count=%d base_vertex=%d: %s; bisect:%s;%s", d->vs_key, d->fs_key, d->primitive, d->count, d->base_vertex, error, variants, spans);
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
        pass.colorAttachments[i].texture = t; pass.colorAttachments[i].level = level;
        if (t.textureType == MTLTextureType3D) pass.colorAttachments[i].depthPlane = slice; else pass.colorAttachments[i].slice = slice;
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
    // Keyed on the effective descriptor values (write masks, blending on/off), not raw state bytes: an unset (zeroed)
    // channel mask and a bound mask of 0 must not share a pipeline.
    struct { void *vs, *fs; uint32_t formats[10]; uint64_t blend[8]; uint8_t write_mask[8], blending[8]; uint32_t attr[16][3]; uint32_t stream[16][2]; } pk;
    memset(&pk, 0, sizeof pk); pk.vs = d->vs; pk.fs = d->fs;
    for (int i = 0; i < 8; i++) { pk.formats[i] = (uint32_t)pd.colorAttachments[i].pixelFormat; memcpy(&pk.blend[i], &d->blend[i], sizeof d->blend[i]); }
    pk.formats[8] = (uint32_t)pd.depthAttachmentPixelFormat; pk.formats[9] = (uint32_t)pd.stencilAttachmentPixelFormat;
    for (int i = 0; i < 8; i++) { pk.write_mask[i] = (uint8_t)pd.colorAttachments[i].writeMask; pk.blending[i] = pd.colorAttachments[i].blendingEnabled;
                                   if (!pk.blending[i]) pk.blend[i] = 0; }
    for (int i = 0; i < d->nattr; i++) { pk.attr[i][0] = d->attr[i].format; pk.attr[i][1] = d->attr[i].offset; pk.attr[i][2] = d->attr[i].stream | (uint32_t)d->vr->attr[i].location << 8; }
    for (int s = 0; s < 16; s++) if (d->stream_mask >> s & 1) { pk.stream[s][0] = d->stream[s].stride; pk.stream[s][1] = d->stream[s].divisor; }
    NSData *pipeline_key = [NSData dataWithBytes:&pk length:sizeof pk];
    if (!pipelines) { pipelines = [NSMutableDictionary dictionary]; depth_states = [NSMutableDictionary dictionary]; }
    id<MTLRenderPipelineState> pso = pipelines[pipeline_key];
    if (!pso) {
        __block NSError *error = nil; __block id<MTLRenderPipelineState> built = nil;
        on_host_stack(^{ NSError *e = nil; built = [device newRenderPipelineStateWithDescriptor:pd error:&e]; error = e; }); pso = built;
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
            textures[stage][i] = bisect_dummy_textures ? dummy_texture() : sampled_texture(d, stage, slot); sampler_states[stage][i] = sampler_state(d, &d->tex[stage][slot].sampler);
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
    // Translated shaders declare every c[n] as 64 KiB; Maxwell returns 0 past the bound size, Metal would read the next pool
    // bytes (or past the pool's MTLBuffer). Each read uniform buffer is copied on the GPU, in command order (after any
    // UpdateUniformBuffer blits already in this command buffer), into a zero-filled 64 KiB buffer.
    id<MTLBuffer> ubo_copies[2][14] = {{nil}};
    id<MTLBlitCommandEncoder> ubo_blit = nil;
    for (int stage = 0; stage < 2; stage++) for (int i = 0; i < refl[stage]->ncbuf; i++) {
        int slot = refl[stage]->cbuf[i].slot; const struct wonder_host_span *u = &d->ubo[stage][slot];
        id<MTLBuffer> src = (__bridge id<MTLBuffer>)u->buffer; NSUInteger n = MIN((NSUInteger)u->size, (NSUInteger)0x10000);
        if (!src || u->offset + n > src.length) { wonder_host_replay_unlock(); unsupported(d, "uniform buffer span outside its pool buffer, slot", slot); }
        if (!ubo_blit) ubo_blit = [cb blitCommandEncoder];
        id<MTLBuffer> copy = [device newBufferWithLength:0x10000 options:MTLResourceStorageModePrivate];
        [ubo_blit fillBuffer:copy range:NSMakeRange(0, 0x10000) value:0];
        if (n) [ubo_blit copyFromBuffer:src sourceOffset:u->offset toBuffer:copy destinationOffset:0 size:n];
        ubo_copies[stage][slot] = copy;
    }
    // Streams the draw reads past their bound size: zero-filled copies (Maxwell returns zeros there).
    id<MTLBuffer> stream_copies[16] = {nil};
    for (int s = 0; s < 16; s++) if ((d->stream_mask >> s & 1) && d->stream[s].fetch_size > d->stream[s].span.size) {
        id<MTLBuffer> src = (__bridge id<MTLBuffer>)d->stream[s].span.buffer;
        if (!src || d->stream[s].span.offset + d->stream[s].span.size > src.length) { wonder_host_replay_unlock(); unsupported(d, "vertex stream span outside its pool buffer, stream", s); }
        if (!ubo_blit) ubo_blit = [cb blitCommandEncoder];
        NSUInteger n = (NSUInteger)((d->stream[s].fetch_size + 15) & ~15ull);
        id<MTLBuffer> copy = [device newBufferWithLength:n options:MTLResourceStorageModePrivate];
        [ubo_blit fillBuffer:copy range:NSMakeRange(0, n) value:0];
        if (d->stream[s].span.size) [ubo_blit copyFromBuffer:src sourceOffset:d->stream[s].span.offset toBuffer:copy destinationOffset:0 size:d->stream[s].span.size];
        stream_copies[s] = copy;
    }
    [ubo_blit endEncoding];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
    if (!e) { wonder_host_replay_unlock(); unsupported(d, "render encoder creation", 0); }
    e.label = [NSString stringWithFormat:@"NVN draw vs=%s fs=%s", d->vs_key, d->fs_key];
    [e setRenderPipelineState:pso]; [e setDepthStencilState:dss];
    [e setBlendColorRed:d->blend_color[0] green:d->blend_color[1] blue:d->blend_color[2] alpha:d->blend_color[3]];
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
        for (int i = 0; i < r->ncbuf; i++) buffer(ubo_copies[stage][r->cbuf[i].slot], 0, r->cbuf[i].buffer);
        if (r->c1_buffer >= 0) buffer(c1_buffers[stage], 0, (NSUInteger)r->c1_buffer);
        if (r->support >= 0) buffer(support_buffer, 0, (NSUInteger)r->support);
        for (int i = 0; i < r->ntex; i++) {
            if (stage) { [e setFragmentTexture:textures[stage][i] atIndex:r->tex[i].texture]; [e setFragmentSamplerState:sampler_states[stage][i] atIndex:r->tex[i].sampler]; }
            else { [e setVertexTexture:textures[stage][i] atIndex:r->tex[i].texture]; [e setVertexSamplerState:sampler_states[stage][i] atIndex:r->tex[i].sampler]; }
        }
    }
    for (int s = 0; s < 16; s++) if (d->stream_mask >> s & 1) {
        if (stream_copies[s]) [e setVertexBuffer:stream_copies[s] offset:0 atIndex:WONDER_SHADER_STREAM_BUFFER_BASE + s];
        else [e setVertexBuffer:(__bridge id<MTLBuffer>)d->stream[s].span.buffer offset:d->stream[s].span.offset atIndex:WONDER_SHADER_STREAM_BUFFER_BASE + s];
    }
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
    if (getenv("WONDER_DRAW_SYNC")) sync_check(d);
    return 0;
}

NSRange wonder_host_view_slices(MTLTextureType type, NSUInteger array_length, NSUInteger first, NSUInteger count) {
    int cube = type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray;
    NSUInteger total = type == MTLTextureTypeCube ? 6 : type == MTLTextureTypeCubeArray ? 6 * array_length : type == MTLTextureType2DArray ? array_length : 1;
    if (!count) count = first < total ? total - first : 0;
    if (cube) { if (first % 6) return NSMakeRange(0, 0); count = (count + 5) / 6 * 6; }
    if (first >= total || count > total - first || !count) return NSMakeRange(0, 0);
    return NSMakeRange(first, count);
}
MTLTextureDescriptor *wonder_host_render_target_descriptor(const nvn_texdesc *t, MTLPixelFormat format) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    NSUInteger depth = t->depth ? t->depth : 1;
    switch (t->target) {
    case 1: case 7: d.textureType = MTLTextureType2D; break;
    case 4: d.textureType = MTLTextureType2DArray; d.arrayLength = depth; break;
    case 8: if (depth != 6 && depth != 1) return nil; d.textureType = MTLTextureTypeCube; break;
    case 9: if (depth % 6) return nil; d.textureType = MTLTextureTypeCubeArray; d.arrayLength = depth / 6; break;
    case 2: d.textureType = MTLTextureType3D; d.depth = depth; break;   // rendered one z slice at a time (depthPlane)
    default: return nil;
    }
    d.pixelFormat = format; d.width = t->width; d.height = t->height; d.mipmapLevelCount = t->levels ? t->levels : 1;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView; d.storageMode = MTLStorageModePrivate;
    return d;
}
// ---------- Scissored / masked colour clears ----------
static NSString *const clear_source = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; };\n"
    "vertex V clear_vs(uint id [[vertex_id]]) { V o; o.p = float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, 0.0, 1.0); return o; }\n"
    "fragment float4 clear_fs(constant float4 &c [[buffer(0)]]) { return c; }\n";
int wonder_host_clear_rect(void *texture, int level, int slice, const float rgba[4], unsigned mask, const int rect[4]) {
    id<MTLTexture> t = (__bridge id<MTLTexture>)texture; if (!t || !device) return 1;
    static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *pipelines_by_key; static id<MTLLibrary> library;
    if (!library) { __block id<MTLLibrary> l = nil; on_host_stack(^{ l = [device newLibraryWithSource:clear_source options:nil error:NULL]; }); library = l; if (!library) return 2; }
    MTLPixelFormat f = t.pixelFormat;
    MTLColorWriteMask wm = (mask & 1 ? MTLColorWriteMaskRed : 0) | (mask & 2 ? MTLColorWriteMaskGreen : 0) | (mask & 4 ? MTLColorWriteMaskBlue : 0) | (mask & 8 ? MTLColorWriteMaskAlpha : 0);
    NSNumber *key = @((NSUInteger)f << 4 | wm);
    if (!pipelines_by_key) pipelines_by_key = [NSMutableDictionary dictionary];
    id<MTLRenderPipelineState> pso = pipelines_by_key[key];
    if (!pso) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [library newFunctionWithName:@"clear_vs"]; pd.fragmentFunction = [library newFunctionWithName:@"clear_fs"];
        pd.colorAttachments[0].pixelFormat = f; pd.colorAttachments[0].writeMask = wm;
        __block id<MTLRenderPipelineState> built = nil; on_host_stack(^{ built = [device newRenderPipelineStateWithDescriptor:pd error:NULL]; }); pso = built;
        if (!pso) return 3;   // e.g. integer formats
        pipelines_by_key[key] = pso;
    }
    NSUInteger w = MAX((NSUInteger)1, t.width >> level), h = MAX((NSUInteger)1, t.height >> level);
    long x0 = MAX(0, rect[0]), y0 = MAX(0, rect[1]), x1 = MIN((long)w, (long)rect[0] + rect[2]), y1 = MIN((long)h, (long)rect[1] + rect[3]);
    if (x1 <= x0 || y1 <= y0 || !wm) return 0;   // nothing to clear
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = t; p.colorAttachments[0].level = level;
    if (t.textureType == MTLTextureType3D) p.colorAttachments[0].depthPlane = slice; else p.colorAttachments[0].slice = slice;
    p.colorAttachments[0].loadAction = MTLLoadActionLoad; p.colorAttachments[0].storeAction = MTLStoreActionStore;
    wonder_host_replay_lock();
    id<MTLRenderCommandEncoder> e = [(__bridge id<MTLCommandBuffer>)wonder_host_replay_command_buffer() renderCommandEncoderWithDescriptor:p];
    [e setRenderPipelineState:pso];
    [e setScissorRect:(MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)}];
    [e setFragmentBytes:rgba length:16 atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    wonder_host_replay_unlock();
    return 0;
}

static NSString *const clear_depth_source = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; };\n"
    "vertex V clear_depth_vs(uint id [[vertex_id]], constant float &z [[buffer(0)]]) { V o; o.p = float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, z, 1.0); return o; }\n"
    "fragment void clear_depth_fs() {}\n";
int wonder_host_clear_depth_stencil_rect(void *texture, int level, int slice, int clear_depth, float depth, int clear_stencil,
                                         uint32_t stencil, uint32_t stencil_mask, const int rect[4]) {
    id<MTLTexture> t = (__bridge id<MTLTexture>)texture; if (!t || !device) return 1;
    MTLPixelFormat f = t.pixelFormat;
    int has_depth = f != MTLPixelFormatStencil8, has_stencil = f == MTLPixelFormatStencil8 || f == MTLPixelFormatDepth32Float_Stencil8;
    clear_depth = clear_depth && has_depth; clear_stencil = clear_stencil && has_stencil && (stencil_mask & 0xff);
    if (!clear_depth && !clear_stencil) return 0;
    static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *pipelines_by_format; static id<MTLLibrary> library;
    if (!library) { __block id<MTLLibrary> l = nil; on_host_stack(^{ l = [device newLibraryWithSource:clear_depth_source options:nil error:NULL]; }); library = l; if (!library) return 2; }
    if (!pipelines_by_format) pipelines_by_format = [NSMutableDictionary dictionary];
    id<MTLRenderPipelineState> pso = pipelines_by_format[@(f)];
    if (!pso) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = [library newFunctionWithName:@"clear_depth_vs"];
        pd.fragmentFunction = [library newFunctionWithName:@"clear_depth_fs"];
        if (has_depth) pd.depthAttachmentPixelFormat = f;
        if (has_stencil) pd.stencilAttachmentPixelFormat = f;
        __block id<MTLRenderPipelineState> built = nil; on_host_stack(^{ built = [device newRenderPipelineStateWithDescriptor:pd error:NULL]; }); pso = built;
        if (!pso) return 3;
        pipelines_by_format[@(f)] = pso;
    }
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = clear_depth;
    if (clear_stencil) {
        MTLStencilDescriptor *sd = [MTLStencilDescriptor new]; sd.stencilCompareFunction = MTLCompareFunctionAlways;
        sd.depthStencilPassOperation = MTLStencilOperationReplace; sd.writeMask = stencil_mask & 0xff; sd.readMask = 0xff;
        dd.frontFaceStencil = sd; dd.backFaceStencil = sd;
    }
    id<MTLDepthStencilState> dss = [device newDepthStencilStateWithDescriptor:dd];
    NSUInteger w = MAX((NSUInteger)1, t.width >> level), h = MAX((NSUInteger)1, t.height >> level);
    long x0 = MAX(0, rect[0]), y0 = MAX(0, rect[1]), x1 = MIN((long)w, (long)rect[0] + rect[2]), y1 = MIN((long)h, (long)rect[1] + rect[3]);
    if (x1 <= x0 || y1 <= y0) return 0;
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    if (has_depth) { p.depthAttachment.texture = t; p.depthAttachment.level = level; p.depthAttachment.slice = slice;
                     p.depthAttachment.loadAction = MTLLoadActionLoad; p.depthAttachment.storeAction = MTLStoreActionStore; }
    if (has_stencil) { p.stencilAttachment.texture = t; p.stencilAttachment.level = level; p.stencilAttachment.slice = slice;
                       p.stencilAttachment.loadAction = MTLLoadActionLoad; p.stencilAttachment.storeAction = MTLStoreActionStore; }
    wonder_host_replay_lock();
    id<MTLRenderCommandEncoder> e = [(__bridge id<MTLCommandBuffer>)wonder_host_replay_command_buffer() renderCommandEncoderWithDescriptor:p];
    [e setRenderPipelineState:pso]; [e setDepthStencilState:dss]; [e setStencilReferenceValue:stencil & 0xff];
    [e setViewport:(MTLViewport){0, 0, (double)w, (double)h, 0, 1}];
    [e setScissorRect:(MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)}];
    [e setVertexBytes:&depth length:4 atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
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
