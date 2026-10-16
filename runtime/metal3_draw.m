#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
#include <mach/mach.h>
#include <stdio.h>
#include <unistd.h>
#include <stdatomic.h>
#include <TargetConditionals.h>
#include "metal3_draw.h"
#include "host_draw.h"
#include "tbdr_plan.h"
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
    if (stage >= 0 && ![entry[@"stage"] isEqual:stage == 5 ? @"cs" : stage == 1 ? @"ps" : @"vs"])
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
    MTLFunctionType want = stage == 5 ? MTLFunctionTypeKernel : stage ? MTLFunctionTypeFragment : MTLFunctionTypeVertex;
    if (!f) {
        NSData *source = verified(key, @"metal", @"msl_sha256", stage);
        if (!device) wonder_nvn_stop("NVN_SHADER_COMPILE_FAILED key=%s: no Metal device", key_c);
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion3_2; options.mathMode = MTLMathModeSafe;   // no fast math: keep the translator's precision contract
        options.preserveInvariance = YES;   // [[position, invariant]] (wonder_shader_build.py): depth pre-pass + depth EQUAL passes
        __block NSError *error = nil; __block id<MTLLibrary> library = nil;
        on_host_stack(^{ NSError *e = nil; library = [device newLibraryWithSource:[[NSString alloc] initWithData:source encoding:NSUTF8StringEncoding] options:options error:&e]; error = e; });
        if (!library) {
            NSString *first = nil;   // the compiler's own "program_source:L:C: error:" line when there is one
            for (NSString *line in [error.localizedDescription componentsSeparatedByString:@"\n"]) if ([line containsString:@"error:"]) { first = line; break; }
            if (!first) first = [error.localizedDescription componentsSeparatedByString:@"\n"].firstObject ?: @"?";
            wonder_nvn_stop("NVN_SHADER_COMPILE_FAILED key=%s error=%s", key_c, first.UTF8String);
        }
        f = [library newFunctionWithName:@"main0"];
        if (!f || f.functionType != want) wonder_nvn_stop("NVN_SHADER_STAGE key=%s: main0 is not a %s function", key_c, stage == 5 ? "kernel" : stage ? "fragment" : "vertex");
        f.label = key; functions[key] = f;
    } else if (f.functionType != want)
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
#include "nvn_deswizzle.h"
#include "page_guard.h"
_Noreturn static void unsupported(const struct wonder_host_draw *d, const char *what, long value) {
    wonder_nvn_stop("NVN_DRAW_UNSUPPORTED %s %ld (vs=%s fs=%s)", what, value, d->vs_key, d->fs_key);
}
uint32_t wonder_host_vertex_format(int f) {
    switch (f) {
    case 0x0A: return MTLVertexFormatFloat;   case 0x16: return MTLVertexFormatFloat2;  case 0x22: return MTLVertexFormatFloat3;  case 0x2E: return MTLVertexFormatFloat4;   // R/RG/RGB/RGBA32F
    case 0x3D: return MTLVertexFormatUInt1010102Normalized; case 0x69: return MTLVertexFormatInt1010102Normalized;   // RGB10A2 / RGB10A2SN (round133)
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
static uint64_t fnv64(const uint8_t *p, uint64_t n) { uint64_t h = 1469598103934665603ull; for (uint64_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull; return h; }
// CPU-uploaded copies of sampled textures by serial; nvnTextureFinalize drops them (guest threads, hence the lock).
static NSMutableDictionary<NSNumber *, id<MTLTexture>> *sampled; static NSMutableDictionary<NSNumber *, NSNumber *> *sampled_hash, *sampled_guard;
static _Atomic unsigned sampled_validations;   // hashes of CPU texture storage (diagnostic: HOST_MEMORY)
unsigned wonder_host_draw_sampled_validations(void) { return atomic_load(&sampled_validations); }
static pthread_mutex_t sampled_lock = PTHREAD_MUTEX_INITIALIZER;
void wonder_host_draw_forget_sampled(uint64_t serial) {
    pthread_mutex_lock(&sampled_lock); uint32_t guard = [sampled_guard[@(serial)] unsignedIntValue];
    [sampled removeObjectForKey:@(serial)]; [sampled_hash removeObjectForKey:@(serial)]; [sampled_guard removeObjectForKey:@(serial)]; pthread_mutex_unlock(&sampled_lock);
    if (guard) wonder_guard_forget(guard);
}
unsigned wonder_host_draw_sampled_count(void) { pthread_mutex_lock(&sampled_lock); unsigned n = (unsigned)sampled.count; pthread_mutex_unlock(&sampled_lock); return n; }
static id<MTLTexture> sampled_texture(const struct wonder_host_draw *d, int stage, int slot) {
    const struct wonder_nvn_texture *t = &d->tex[stage][slot].texture.texture;
    const struct wonder_nvn_texture_registration *reg = &d->tex[stage][slot].texture;
    // Texture views (registered with the pool entry): same format and target as the texture, a level/layer sub-range and a
    // swizzle are executed as a Metal texture view; reinterpreting the format or target stops by name.
    const struct wonder_nvn_texture_view *v = reg->has_view ? &reg->view : NULL;
    if (v && v->set_format && v->format != t->desc.format) unsupported(d, "texture view format (reinterpretation) on serial", (long)t->serial);
    if (v && v->set_depth_stencil && v->depth_stencil_mode && t->desc.format >= 0x31 && t->desc.format <= 0x36) unsupported(d, "stencil texturing view on serial", (long)t->serial);
    id<MTLTexture> gpu = (__bridge id<MTLTexture>)wonder_host_render_target_existing(t);
    id<MTLTexture> tex = gpu;
    // Pitch-linear, single-level 2D textures are views of the pool's own MTLBuffer: CPU writes (the font glyph cache,
    // 16 MiB, rewritten as text appears) reach the GPU without an upload, and nothing is hashed per draw (hashing it
    // on every text draw stalled every screen with text, round128).
    if (!tex && nvn_tex_is_linear(&t->desc) && (t->desc.target == 1 || t->desc.target == 7) && t->desc.levels <= 1 && d->tex[stage][slot].storage_buffer) {
        NSNumber *key = @(t->serial);
        pthread_mutex_lock(&sampled_lock); if (!sampled) { sampled = [NSMutableDictionary dictionary]; sampled_hash = [NSMutableDictionary dictionary]; }
        tex = sampled[key]; pthread_mutex_unlock(&sampled_lock);
        if (!tex) {
            uint32_t mtl = 0;
            if (nvn_fmt_to_mtl(t->desc.format, &mtl, NULL) == 1) {
                int bw, bh, bpb = nvn_fmt_bpb(t->desc.format, &bw, &bh);
                uint32_t row = (uint32_t)((t->desc.width + (uint32_t)bw - 1) / (uint32_t)bw * (uint32_t)bpb), stride = t->desc.stride ? t->desc.stride : row;
                NSUInteger align = [device minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)mtl];
                uint64_t offset = d->tex[stage][slot].storage_buffer_offset;
                if (bw == 1 && bh == 1 && stride >= row && align && offset % align == 0 && stride % align == 0 &&
                    (uint64_t)stride * t->desc.height <= d->tex[stage][slot].storage_size) {
                    id<MTLBuffer> pool = (__bridge id<MTLBuffer>)d->tex[stage][slot].storage_buffer;
                    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)mtl width:t->desc.width height:t->desc.height mipmapped:NO];
                    td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView; td.storageMode = pool.storageMode; td.cpuCacheMode = pool.cpuCacheMode; td.hazardTrackingMode = pool.hazardTrackingMode;
                    tex = [pool newTextureWithDescriptor:td offset:offset bytesPerRow:stride];
                    if (tex) {
                        tex.label = [NSString stringWithFormat:@"NVN linear %llu %ux%u fmt %#x (pool buffer)", (unsigned long long)t->serial, t->desc.width, t->desc.height, t->desc.format];
                        pthread_mutex_lock(&sampled_lock); sampled[key] = tex; pthread_mutex_unlock(&sampled_lock);
                        fprintf(stderr, "HOST_TEXTURE_LINEAR_ZERO_COPY serial=%llu %ux%u fmt=%#x stride=%u\n", (unsigned long long)t->serial, t->desc.width, t->desc.height, t->desc.format, stride);
                    }
                }
            }
        }
    }
    if (!tex) {
        int target = t->desc.target, linear = nvn_tex_is_linear(&t->desc);   // NVN_TEXTURE_FLAGS_LINEAR / LINEAR_RENDER_TARGET: pitch layout, desc.stride bytes per row
        if (target != 1 && target != 2 && target != 4 && target != 7) unsupported(d, "sampled texture target (2D / 3D / 2D array / rectangle so far)", target);
        if (linear && (target == 2 || target == 4 || t->desc.levels > 1)) unsupported(d, "pitch-linear texture with layers, depth or mips, serial", (long)t->serial);
        if (!d->tex[stage][slot].storage) unsupported(d, "sampled texture without CPU storage, serial", (long)t->serial);
        // BC1-7 map exactly and are sampled natively (the graphics gate requires supportsBCTextureCompression).
        uint32_t mtl = 0; int fit = nvn_fmt_to_mtl(t->desc.format, &mtl, NULL);
        if (fit != 1) unsupported(d, "sampled texture format", t->desc.format);
        // Re-validated only after a CPU write to its pool pages (page_guard.h): protect first, then hash, so a write in
        // between still marks it dirty for the next draw.
        NSNumber *key = @(t->serial);
        pthread_mutex_lock(&sampled_lock);
        if (!sampled) { sampled = [NSMutableDictionary dictionary]; sampled_hash = [NSMutableDictionary dictionary]; sampled_guard = [NSMutableDictionary dictionary]; }
        tex = sampled[key]; uint32_t guard = [sampled_guard[key] unsignedIntValue]; NSNumber *known = sampled_hash[key];
        pthread_mutex_unlock(&sampled_lock);
        int stale = 0; uint64_t hash = 0;
        if (!tex || !guard || wonder_guard_dirty(guard)) {
            if (guard) wonder_guard_rearm(guard);
            else if ((guard = wonder_guard_watch(d->tex[stage][slot].storage, (size_t)d->tex[stage][slot].storage_size))) {
                pthread_mutex_lock(&sampled_lock); sampled_guard[key] = @(guard); pthread_mutex_unlock(&sampled_lock);
            }
            hash = fnv64(d->tex[stage][slot].storage, d->tex[stage][slot].storage_size);
            stale = !tex || known.unsignedLongLongValue != hash;
            atomic_fetch_add(&sampled_validations, 1);
        }
        if (stale) {
            MTLPixelFormat pf = (MTLPixelFormat)mtl;
            NSUInteger layers = target == 4 ? (t->desc.depth ? t->desc.depth : 1) : 1;
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:t->desc.width height:t->desc.height mipmapped:t->desc.levels > 1];
            if (target == 4) { td.textureType = MTLTextureType2DArray; td.arrayLength = layers; }
            if (target == 2) { td.textureType = MTLTextureType3D; td.depth = t->desc.depth ? t->desc.depth : 1; }   // round139: compute 1fb9d970 samples a 3D table
            td.mipmapLevelCount = t->desc.levels ? t->desc.levels : 1; td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView; td.storageMode = CPU_TEXTURE_STORAGE;
            tex = [device newTextureWithDescriptor:td];
            if (!tex) unsupported(d, "sampled texture allocation, serial", (long)t->serial);
            for (NSUInteger layer = 0; layer < layers; layer++) for (unsigned level = 0; level < td.mipmapLevelCount; level++) {
                uint32_t w = t->desc.width >> level ? t->desc.width >> level : 1, h = t->desc.height >> level ? t->desc.height >> level : 1;
                const uint8_t *src; uint32_t row_bytes; NSMutableData *linear_copy = nil;
                if (linear) {   // rows of desc.stride bytes straight from the pool
                    int bw, bh, bpb = nvn_fmt_bpb(t->desc.format, &bw, &bh);
                    row_bytes = (uint32_t)((w + (uint32_t)bw - 1) / (uint32_t)bw * (uint32_t)bpb);
                    uint32_t rows = (h + (uint32_t)bh - 1) / (uint32_t)bh, stride = t->desc.stride ? t->desc.stride : row_bytes;
                    if (stride < row_bytes || (uint64_t)stride * (rows - 1) + row_bytes > d->tex[stage][slot].storage_size) unsupported(d, "pitch-linear texture beyond storage, serial", (long)t->serial);
                    src = d->tex[stage][slot].storage; row_bytes = stride;
                } else {
                    uint32_t rows, slices, lbpb; uint64_t base; unsigned th, tdp;
                    if (nvn_deswizzle_geom(&t->desc, level, (unsigned)layer, &row_bytes, &rows, &slices, &base, &th, &tdp, &lbpb)) unsupported(d, "texture layout level", level);
                    if (base + (uint64_t)row_bytes * rows > d->tex[stage][slot].storage_size) unsupported(d, "texture level beyond storage, level", level);
                    if (target == 2) {   // 3D: every depth slice of the level at once, slice k at k * row_bytes * rows
                        if (base + (uint64_t)row_bytes * rows * slices > d->tex[stage][slot].storage_size) unsupported(d, "3D texture level beyond storage, level", level);
                        linear_copy = [NSMutableData dataWithLength:(NSUInteger)row_bytes * rows * slices];
                        if (nvn_deswizzle(&t->desc, level, 0, d->tex[stage][slot].storage, linear_copy.mutableBytes, row_bytes)) unsupported(d, "3D texture deswizzle level", level);
                        NSUInteger dl = MAX((NSUInteger)1, (NSUInteger)td.depth >> level);
                        if (slices != dl) unsupported(d, "3D texture slice count differs from the level depth, level", level);
                        [tex replaceRegion:MTLRegionMake3D(0, 0, 0, w, h, dl) mipmapLevel:level slice:0 withBytes:linear_copy.bytes bytesPerRow:row_bytes bytesPerImage:(NSUInteger)row_bytes * rows];
                        continue;
                    }
                    linear_copy = [NSMutableData dataWithLength:(NSUInteger)row_bytes * rows];
                    if (nvn_deswizzle(&t->desc, level, (unsigned)layer, d->tex[stage][slot].storage, linear_copy.mutableBytes, row_bytes)) unsupported(d, "texture deswizzle level", level);
                    src = linear_copy.bytes;
                }
                [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level slice:layer withBytes:src bytesPerRow:row_bytes bytesPerImage:0];
            }
            tex.label = [NSString stringWithFormat:@"NVN sampled %llu %ux%u fmt %#x", (unsigned long long)t->serial, t->desc.width, t->desc.height, t->desc.format];
            pthread_mutex_lock(&sampled_lock); sampled[key] = tex; sampled_hash[key] = @(hash); pthread_mutex_unlock(&sampled_lock);
        }
    }
    NSUInteger levels = tex.mipmapLevelCount, base = 0, count = levels;
    NSRange all = wonder_host_view_slices(tex.textureType, tex.arrayLength, 0, 0), range = all;
    if (v && v->set_levels) { base = v->base_level; count = v->levels ? v->levels : levels - base; }
    // A view with its own target (round149: a 2D RGBA8_SRGB texture viewed as a 2D array for a texture2d_array shader)
    // becomes a Metal view of that type when Metal allows it and the layers fit; anything else stops by name.
    MTLTextureType type = tex.textureType;
    if (v && v->set_target && v->target != t->desc.target) {
        long vt = wonder_host_view_type(tex.textureType, v->target);
        if (vt < 0) unsupported(d, "texture view target on serial", (long)t->serial);
        type = (MTLTextureType)vt;
    }
    if (type != tex.textureType || (v && v->set_layers)) {
        range = wonder_host_view_type_slices(tex.textureType, tex.arrayLength, type, v && v->set_layers ? (NSUInteger)MAX(0, v->min_layer) : 0,
                                             v && v->set_layers ? (NSUInteger)MAX(0, v->layers) : 0);
    }
    if (base >= levels || count > levels - base) unsupported(d, "texture view levels beyond the texture, serial", (long)t->serial);
    if (!range.length) unsupported(d, "texture view layers not representable (cube views need whole cubes, 2D views one layer), serial", (long)t->serial);
    NSUInteger first = range.location, layers = range.length, slices = all.length;
    const uint8_t *z = v && v->set_swizzle ? v->swizzle : t->swizzle;   // NVN swizzle values equal MTLTextureSwizzle (ZERO 0, ONE 1, R 2, G 3, B 4, A 5)
    if (t->desc.format == 0x1 && (t->desc.flags & 0x10)) {   // diagnostic: the font glyph cache (2048x8192 R8 linear, round123 serial 38)
        static _Atomic int logged; int n = atomic_fetch_add(&logged, 1);
        if (n < 16 || n % 2000 == 0) {
            size_t nonzero = 0; const uint8_t *st = d->tex[stage][slot].storage;
            for (size_t i = 0; st && i < d->tex[stage][slot].storage_size; i += 64) nonzero += st[i] != 0;
            fprintf(stderr, "HOST_GLYPH_SAMPLE #%d vs=%s fs=%s serial=%llu %ux%u stride=%u swizzle=%u,%u,%u,%u view=%d storage=%llu nonzero/64=%zu gpu=%d\n", n, d->vs_key, d->fs_key,
                    (unsigned long long)t->serial, t->desc.width, t->desc.height, t->desc.stride, z[0], z[1], z[2], z[3], v != NULL, (unsigned long long)d->tex[stage][slot].storage_size, nonzero, gpu != nil);
        }
    }
    int identity = z[0] == 2 && z[1] == 3 && z[2] == 4 && z[3] == 5, unset = !(z[0] | z[1] | z[2] | z[3]);
    if (!identity && !unset && (z[0] > 5 || z[1] > 5 || z[2] > 5 || z[3] > 5)) unsupported(d, "texture swizzle", z[0] | z[1] << 8 | z[2] << 16 | z[3] << 24);
    int whole = base == 0 && count == levels && first == 0 && layers == slices && type == tex.textureType;
    if (whole && (identity || unset)) return tex;
    id<MTLTexture> view = identity || unset   // the swizzle-taking call fails outright on GPUs without texture swizzle
        ? [tex newTextureViewWithPixelFormat:tex.pixelFormat textureType:type levels:NSMakeRange(base, count) slices:NSMakeRange(first, layers)]
        : [tex newTextureViewWithPixelFormat:tex.pixelFormat textureType:type levels:NSMakeRange(base, count) slices:NSMakeRange(first, layers)
                                     swizzle:MTLTextureSwizzleChannelsMake(z[0], z[1], z[2], z[3])];
    if (!view) unsupported(d, "texture view creation on serial", (long)t->serial);
    return view;
}
// NVN_WRAP_MODE_CLAMP_TO_BORDER: Metal offers transparent black, opaque black and opaque white only.
int wonder_host_border_color(const float c[4]) {
    if (c[0] == 0 && c[1] == 0 && c[2] == 0) return c[3] == 0 ? (int)MTLSamplerBorderColorTransparentBlack : c[3] == 1 ? (int)MTLSamplerBorderColorOpaqueBlack : -1;
    if (c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1) return (int)MTLSamplerBorderColorOpaqueWhite;
    return -1;
}
// Transparent black is exactly ClampToZero; opaque black / white use the border colour (every Metal 3 GPU has it).
int wonder_host_border_address_mode(const float c[4]) {
    int color = wonder_host_border_color(c);
    if (color < 0) return -1;
    return color == (int)MTLSamplerBorderColorTransparentBlack ? (int)MTLSamplerAddressModeClampToZero : (int)MTLSamplerAddressModeClampToBorderColor;
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
    const uint8_t wraps[3] = {m->wrap_s, m->wrap_t, m->wrap_r}; MTLSamplerAddressMode modes[3]; int border = 0;
    for (int i = 0; i < 3; i++) switch (wraps[i]) {
        case 0: case 7: modes[i] = MTLSamplerAddressModeClampToEdge; break;   // CLAMP (legacy GL clamp) treated as clamp-to-edge
        case 1: modes[i] = MTLSamplerAddressModeRepeat; break;
        case 2: case 3: modes[i] = MTLSamplerAddressModeMirrorClampToEdge; break;
        case 6: modes[i] = MTLSamplerAddressModeMirrorRepeat; break;
        case 5: border = 1; break;   // CLAMP_TO_BORDER, mode chosen below
        default: unsupported(d, "sampler wrap mode (MIRROR_CLAMP_TO_BORDER has no Metal mode)", wraps[i]);
    }
    if (border) {
        int color = wonder_host_border_color(m->border);
        if (color < 0) {
            char what[96]; snprintf(what, sizeof what, "sampler border colour %g,%g,%g,%g (Metal: transparent/opaque black, opaque white)", m->border[0], m->border[1], m->border[2], m->border[3]);
            unsupported(d, what, 5);
        }
        int mode = wonder_host_border_address_mode(m->border);
        sd.borderColor = (MTLSamplerBorderColor)color;
        for (int i = 0; i < 3; i++) if (wraps[i] == 5) modes[i] = (MTLSamplerAddressMode)mode;
    }
    sd.sAddressMode = modes[0]; sd.tAddressMode = modes[1]; sd.rAddressMode = modes[2];
    sd.lodMinClamp = m->lod_min; sd.lodMaxClamp = m->lod_max;
    if (m->lod_bias != 0) {   // round130: -1.5 on a course material; MTLSamplerDescriptor.lodBias exists from iOS / macOS 26
        if (@available(iOS 26.0, macOS 26.0, *)) {
            if (m->lod_bias < -16.0f || m->lod_bias > 15.99f) unsupported(d, "sampler LOD bias outside Metal's range x1000", (long)(m->lod_bias * 1000));
            sd.lodBias = m->lod_bias;
        } else unsupported(d, "sampler LOD bias (needs iOS 26) x1000", (long)(m->lod_bias * 1000));
    }
    if (m->reduction) {   // round131: MAX (2) on a course pass; NVN AVERAGE 0 / MIN 1 / MAX 2 equal MTLSamplerReductionMode (iOS 26+)
        if (m->reduction > 2) unsupported(d, "sampler reduction filter", m->reduction);
        if (@available(iOS 26.0, macOS 26.0, *)) sd.reductionMode = (MTLSamplerReductionMode)m->reduction;
        else unsupported(d, "sampler reduction filter (needs iOS 26)", m->reduction);
    }
    sd.maxAnisotropy = m->anisotropy >= 1 ? (NSUInteger)m->anisotropy : 1;
    sd.supportArgumentBuffers = YES;   // samplers 16.. are referenced from an argument buffer (smpov)
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
// Handle-0 texture slots (host_draw.h empty): an all-zero 1x1 texture of the shader's declared type (every translated
// shader samples float textures) and a fixed nearest / clamp sampler.
static id<MTLTexture> empty_texture(const struct wonder_host_draw *d, int type) {
    static id<MTLTexture> cache[16]; if (type < 0 || type >= 16) unsupported(d, "empty texture slot of shader type", type);
    if (cache[type]) return cache[type];
    MTLTextureDescriptor *td = [MTLTextureDescriptor new]; td.pixelFormat = MTLPixelFormatRGBA8Unorm; td.width = td.height = td.depth = 1;
    td.storageMode = CPU_TEXTURE_STORAGE; td.usage = MTLTextureUsageShaderRead; NSUInteger slices = 1;
    switch (type) {
    case WONDER_SHADER_TEX_2D: td.textureType = MTLTextureType2D; break;
    case WONDER_SHADER_TEX_2D_ARRAY: td.textureType = MTLTextureType2DArray; break;
    case WONDER_SHADER_TEX_3D: td.textureType = MTLTextureType3D; break;
    case WONDER_SHADER_TEX_CUBE: td.textureType = MTLTextureTypeCube; slices = 6; break;
    case WONDER_SHADER_TEX_CUBE_ARRAY: td.textureType = MTLTextureTypeCubeArray; slices = 6; break;
    default: unsupported(d, "empty texture slot of shader type", type);
    }
    id<MTLTexture> t = [device newTextureWithDescriptor:td]; if (!t) unsupported(d, "empty texture allocation, type", type);
    uint32_t zero = 0; for (NSUInteger s = 0; s < slices; s++) [t replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:s withBytes:&zero bytesPerRow:4 bytesPerImage:4];
    t.label = @"NVN empty texture slot (handle 0)"; return cache[type] = t;
}
static id<MTLSamplerState> empty_sampler(void) {
    static id<MTLSamplerState> s; if (s) return s;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.sAddressMode = sd.tAddressMode = sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
    return s = [device newSamplerStateWithDescriptor:sd];
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
    for (int st = 0; st < 2; st++) for (int r = 0; r < 40 && at < sizeof spans - 60; r++) if (d->tex_mask[st] >> r & 1)
        at += (size_t)snprintf(spans + at, sizeof spans - at, " tex%d.%d=serial %llu", st, r, (unsigned long long)d->tex[st][r].texture.texture.serial);
    #undef SPAN
    char variants[256]; bisect(d, variants, sizeof variants);
    wonder_nvn_stop("NVN_DRAW_GPU_FAULT vs=%s fs=%s primitive=%d count=%d base_vertex=%d: %s; bisect:%s;%s", d->vs_key, d->fs_key, d->primitive, d->count, d->base_vertex, error, variants, spans);
}
// ---------- Pass batching ----------
// Consecutive draws into the same attachments share one render pass (one load/store of the attachments instead of one
// per draw: round136 in the course ran 141 single-draw passes per frame at ~6 fps). The batch's buffer copies run in one
// blit encoder ahead of its render encoder. Any other GPU operation takes the pending command buffer through
// wonder_host_replay_command_buffer() (host_graphics.m pending_buffer), which flushes the batch first, so command order
// is kept. All batch state is under the replay lock.
struct draw_copy { __unsafe_unretained id<MTLBuffer> src; NSUInteger src_offset; __unsafe_unretained id<MTLBuffer> dst; NSUInteger dst_offset, size, window; };
struct draw_encoding {
    NSUInteger index[2][32], offset[2][32]; int nb[2];
    uint8_t tex_index[2][40], smp_index[2][40]; int ntex[2];
    int smp_overflow[2]; uint64_t smp_ids[2][32];   // samplers 16..47: MTLResourceIDs set as bytes at [[buffer(smp_overflow)]]
    NSUInteger stream_index[16], stream_offset[16]; int nstreams;
    int index_type, count, first, instances, base_instance, base_vertex, bias, uses_default, depth_clamp;
    float blend_color[4], bias_units, bias_slope, bias_clamp;
    MTLCullMode cull; MTLWinding winding; MTLViewport viewport; MTLScissorRect scissor; MTLPrimitiveType primitive; MTLIndexType itype; NSUInteger ib_offset;
};
static MTLRenderPassDescriptor *batch_pass; static NSMutableArray *batch_blocks, *batch_buffers; static NSMutableData *batch_reads; static struct draw_copy *batch_copies; static int batch_ncopies, batch_cap;
static unsigned batch_draws_total, batch_passes_total;
// Scratch windows for copies: carved from 4 MiB private buffers, never reused (the command buffer retains each chunk
// until it completes), instead of one 64 KiB allocation per uniform buffer per draw.
static id<MTLBuffer> scratch_chunk; static NSUInteger scratch_used;
static id<MTLBuffer> scratch_window(NSUInteger size, NSUInteger *offset) {
    size = (size + 255) & ~(NSUInteger)255;
    if (!scratch_chunk || scratch_used + size > scratch_chunk.length) {
        scratch_chunk = [device newBufferWithLength:MAX(size, (NSUInteger)4 << 20) options:MTLResourceStorageModePrivate]; scratch_used = 0;
        if (!scratch_chunk) wonder_nvn_stop("NVN_DRAW_UNSUPPORTED scratch buffer allocation of %lu bytes failed", (unsigned long)size);
    }
    *offset = scratch_used; scratch_used += size; return scratch_chunk;
}
static int draw_batch_open(void) { return batch_pass != nil; }
static int same_attachment(MTLRenderPassAttachmentDescriptor *a, MTLRenderPassAttachmentDescriptor *b) {
    return a.texture == b.texture && a.level == b.level && a.slice == b.slice && a.depthPlane == b.depthPlane;
}
static int draw_batch_same_pass(MTLRenderPassDescriptor *p) {
    for (int i = 0; i < 8; i++) if (!same_attachment(batch_pass.colorAttachments[i], p.colorAttachments[i])) return 0;
    return same_attachment(batch_pass.depthAttachment, p.depthAttachment) && same_attachment(batch_pass.stencilAttachment, p.stencilAttachment);
}
static id<MTLTexture> texture_root(id<MTLTexture> t) { while (t.parentTexture) t = t.parentTexture; return t; }
static int draw_batch_attachment_sampled_by(MTLRenderPassDescriptor *p, __strong id<MTLTexture> *textures, int n) {
    for (int i = 0; i < n; i++) {
        id<MTLTexture> t = texture_root(textures[i]);
        for (int k = 0; k < 8; k++) if (p.colorAttachments[k].texture && texture_root(p.colorAttachments[k].texture) == t) return 1;
        if ((p.depthAttachment.texture && texture_root(p.depthAttachment.texture) == t) || (p.stencilAttachment.texture && texture_root(p.stencilAttachment.texture) == t)) return 1;
    }
    return 0;
}
static int draw_batch_attachment_sampled(__strong id<MTLTexture> *textures, int n) { return draw_batch_open() && draw_batch_attachment_sampled_by(batch_pass, textures, n); }
static void draw_batch_begin(MTLRenderPassDescriptor *p) {
    batch_pass = p; if (!batch_blocks) { batch_blocks = [NSMutableArray array]; batch_buffers = [NSMutableArray array]; batch_reads = [NSMutableData data]; }
}
// Sampled textures of the batch's draws, as roots (tbdr_plan.c: a pass reading a texture keeps its earlier stores).
static void batch_note_reads(__strong id<MTLTexture> *textures, int n) {
    for (int i = 0; i < n; i++) {
        if (!textures[i]) continue;
        uintptr_t r = (uintptr_t)(__bridge void *)texture_root(textures[i]); const uintptr_t *have = batch_reads.bytes; NSUInteger k = 0, m = batch_reads.length / sizeof r;
        while (k < m && have[k] != r) k++;
        if (k == m) [batch_reads appendBytes:&r length:sizeof r];
    }
}
static void draw_batch_add(const struct draw_copy *c, int n, void (^encode)(id<MTLRenderCommandEncoder>)) {
    if (batch_ncopies + n > batch_cap) {
        batch_cap = MAX(batch_cap * 2, batch_ncopies + n + 64);
        struct draw_copy *grown = realloc(batch_copies, sizeof *grown * (size_t)batch_cap); if (!grown) wonder_nvn_stop("NVN_DRAW_UNSUPPORTED batch copy list allocation failed");
        batch_copies = grown;
    }
    for (int i = 0; i < n; i++) { batch_copies[batch_ncopies++] = c[i]; [batch_buffers addObject:c[i].src]; [batch_buffers addObject:c[i].dst]; }   // retained until the flush
    [batch_blocks addObject:encode];
}
// ---------- Deferred operation queue and load/store planning (tbdr_plan.c) ----------
// A closed batch, a full clear (host_graphics.m wonder_host_clear_color / _depth_stencil) and a buffer fill / update wait in
// a queue. Anything else that needs the command buffer (compute, copies, scissored clears, readback, present, commit)
// takes it through wonder_host_replay_command_buffer(), which first plans and encodes the queue in order, so command
// order is kept and everything after that point counts as unknown (Store). Planning folds each full clear into the next
// pass's Clear load and stores DontCare what the next queued operation overwrites (proof rules in tbdr_plan.h).
// WONDER_TBDR=0 (diagnostic): no planning, every attachment Load/Store and every clear its own pass, as before, for
// comparing frames; WONDER_TBDR=ab alternates on/off per HOST_MEMORY window (300 presents) for in-run fps comparison.
@interface WonderQueuedOp : NSObject { @public int kind; MTLRenderPassDescriptor *pass; NSArray *blocks, *buffers; NSData *copies, *reads; void (^work)(id<MTLCommandBuffer>); }
@end
@implementation WonderQueuedOp
@end
static NSMutableArray<WonderQueuedOp *> *queued;
static int tbdr_mode = -1, tbdr_active;   // mode 0 off, 1 on, 2 ab; active: planning in this window
static struct tbdr_stats tbdr_totals; static unsigned queue_drains, queue_ops_total;
static void tbdr_init(void) {
    if (tbdr_mode >= 0) return;
    const char *e = getenv("WONDER_TBDR"); tbdr_mode = e && !strcmp(e, "0") ? 0 : e && !strcmp(e, "ab") ? 2 : 1; tbdr_active = tbdr_mode != 0;
}
void *wonder_host_replay_command_buffer_raw(void);   // host_graphics.m: the pending command buffer without flushing
static void queue_drain(void);
static void queue_push(WonderQueuedOp *q) {
    if (!queued) queued = [NSMutableArray array];
    [queued addObject:q];
    if (queued.count >= 1024) queue_drain();   // bounded lookahead: the rest of the queue counts as unknown
}
static void batch_close(void) {
    if (!batch_pass) return;
    WonderQueuedOp *q = [WonderQueuedOp new]; q->kind = TBDR_OP_PASS; q->pass = batch_pass; q->blocks = [batch_blocks copy]; q->buffers = [batch_buffers copy];
    q->copies = batch_ncopies ? [NSData dataWithBytes:batch_copies length:sizeof *batch_copies * (size_t)batch_ncopies] : nil; q->reads = [batch_reads copy];
    batch_pass = nil; [batch_blocks removeAllObjects]; [batch_buffers removeAllObjects]; batch_ncopies = 0; batch_reads.length = 0;
    queue_push(q);
}
void wonder_host_draw_queue_clear_locked(void *render_pass) {
    batch_close();
    WonderQueuedOp *q = [WonderQueuedOp new]; q->kind = TBDR_OP_CLEAR; q->pass = (__bridge MTLRenderPassDescriptor *)render_pass; queue_push(q);
}
void wonder_host_draw_queue_work_locked(void (^work)(id<MTLCommandBuffer>)) {
    batch_close();
    WonderQueuedOp *q = [WonderQueuedOp new]; q->kind = TBDR_OP_NEUTRAL; q->work = work; queue_push(q);
}
static void add_att(struct tbdr_op *o, __unsafe_unretained MTLRenderPassAttachmentDescriptor **ad, MTLRenderPassAttachmentDescriptor *a, int plane) {
    if (!a.texture || o->natt >= TBDR_MAX_ATT) return;
    struct tbdr_att *t = &o->att[o->natt]; ad[o->natt++] = a;
    t->object = (uintptr_t)(__bridge void *)a.texture; t->root = (uintptr_t)(__bridge void *)texture_root(a.texture); t->plane = plane;
    t->level = (int)a.level; t->slice = (int)(a.texture.textureType == MTLTextureType3D ? a.depthPlane : a.slice);
    t->load = a.loadAction == MTLLoadActionClear ? TBDR_LOAD_CLEAR : TBDR_LOAD_LOAD;
}
static void encode_pass_op(id<MTLCommandBuffer> cb, WonderQueuedOp *q) {
    const struct draw_copy *copies = q->copies.bytes; NSUInteger ncopies = q->copies.length / sizeof *copies;
    if (ncopies) {
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder]; blit.label = @"NVN draw batch copies";
        for (NSUInteger i = 0; i < ncopies; i++) {
            const struct draw_copy *c = &copies[i];
            if (c->window > c->size) [blit fillBuffer:c->dst range:NSMakeRange(c->dst_offset + c->size, c->window - c->size) value:0];
            if (c->size) [blit copyFromBuffer:c->src sourceOffset:c->src_offset toBuffer:c->dst destinationOffset:c->dst_offset size:c->size];
        }
        [blit endEncoding];
    }
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:q->pass];
    if (!e) wonder_nvn_stop("NVN_DRAW_UNSUPPORTED render encoder creation for a batch of %lu draws", (unsigned long)q->blocks.count);
    e.label = [NSString stringWithFormat:@"NVN pass (%lu draws)", (unsigned long)q->blocks.count];
    for (void (^encode)(id<MTLRenderCommandEncoder>) in q->blocks) encode(e);
    [e endEncoding];
    batch_draws_total += (unsigned)q->blocks.count; batch_passes_total++;
}
static void queue_drain(void) {
    NSUInteger n = queued.count; if (!n) return;
    tbdr_init();
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)wonder_host_replay_command_buffer_raw();
    struct tbdr_op *ops = calloc(n, sizeof *ops);
    __unsafe_unretained MTLRenderPassAttachmentDescriptor *(*ad)[TBDR_MAX_ATT] = (__unsafe_unretained MTLRenderPassAttachmentDescriptor *(*)[TBDR_MAX_ATT])calloc(n, sizeof *ad);
    if (!ops || !ad) wonder_nvn_stop("NVN_DRAW_UNSUPPORTED queue plan allocation of %lu operations failed", (unsigned long)n);
    if (tbdr_active) {
        for (NSUInteger i = 0; i < n; i++) {
            WonderQueuedOp *q = queued[i]; struct tbdr_op *o = &ops[i]; o->kind = q->kind;
            if (q->pass) {
                for (int k = 0; k < 8; k++) add_att(o, ad[i], q->pass.colorAttachments[k], TBDR_PLANE_COLOR);
                add_att(o, ad[i], q->pass.depthAttachment, TBDR_PLANE_DEPTH); add_att(o, ad[i], q->pass.stencilAttachment, TBDR_PLANE_STENCIL);
            }
            o->reads = q->reads.bytes; o->nreads = (int)(q->reads.length / sizeof(uintptr_t));
        }
        tbdr_plan(ops, (int)n, &tbdr_totals);
        for (NSUInteger i = 0; i < n; i++) for (int k = 0; k < ops[i].natt; k++) {
            const struct tbdr_att *t = &ops[i].att[k]; MTLRenderPassAttachmentDescriptor *a = ad[i][k];
            a.storeAction = t->store == TBDR_STORE_DONTCARE ? MTLStoreActionDontCare : MTLStoreActionStore;
            if (ops[i].kind != TBDR_OP_PASS || t->load != TBDR_LOAD_CLEAR) continue;
            MTLRenderPassAttachmentDescriptor *src = ad[t->clear_op][t->clear_att]; a.loadAction = MTLLoadActionClear;
            if (t->plane == TBDR_PLANE_COLOR) ((MTLRenderPassColorAttachmentDescriptor *)a).clearColor = ((MTLRenderPassColorAttachmentDescriptor *)src).clearColor;
            else if (t->plane == TBDR_PLANE_DEPTH) ((MTLRenderPassDepthAttachmentDescriptor *)a).clearDepth = ((MTLRenderPassDepthAttachmentDescriptor *)src).clearDepth;
            else ((MTLRenderPassStencilAttachmentDescriptor *)a).clearStencil = ((MTLRenderPassStencilAttachmentDescriptor *)src).clearStencil;
        }
    }
    for (NSUInteger i = 0; i < n; i++) {
        WonderQueuedOp *q = queued[i];
        if (ops[i].dropped) continue;
        if (q->kind == TBDR_OP_PASS) encode_pass_op(cb, q);
        else if (q->kind == TBDR_OP_CLEAR) {
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:q->pass];
            if (!e) wonder_nvn_stop("NVN_DRAW_UNSUPPORTED render encoder creation for a clear");
            e.label = @"NVN clear"; [e endEncoding];
        } else q->work(cb);
    }
    free(ops); free(ad); queue_drains++; queue_ops_total += (unsigned)n;
    [queued removeAllObjects];
}
void wonder_host_draw_flush_locked(void) { batch_close(); queue_drain(); }
// HOST_MEMORY window report (host_graphics.m, every 300 presents): planning counters since the last call; WONDER_TBDR=ab
// switches planning for the next window.
void wonder_host_tbdr_window(char *out, size_t size) {
    static struct tbdr_stats last; static unsigned last_drains, last_ops;
    wonder_host_replay_lock(); tbdr_init();
    struct tbdr_stats s = tbdr_totals;
    snprintf(out, size, "tbdr=%s drains=%u ops=%u folded_clears=%u dropped_clears=%u dontcare=%u stored=%u", tbdr_active ? "on" : "off",
             queue_drains - last_drains, queue_ops_total - last_ops, s.folded_clears - last.folded_clears, s.dropped_clears - last.dropped_clears,
             s.dontcare_stores - last.dontcare_stores, s.stores - last.stores);
    last = s; last_drains = queue_drains; last_ops = queue_ops_total;
    if (tbdr_mode == 2) tbdr_active = !tbdr_active;
    wonder_host_replay_unlock();
}
void wonder_host_draw_batch_stats(unsigned *draws, unsigned *passes) { *draws = batch_draws_total; *passes = batch_passes_total; }
static NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *pipelines;
static NSMutableDictionary<NSData *, id<MTLDepthStencilState>> *depth_states;
static id<MTLBuffer> support_buffer;
// Diagnostic (WONDER_TEX_DUMP_FS=<fragment key prefix>[,<prefix>...]): the first draw of each listed fragment shader
// (after present WONDER_TEX_DUMP_AFTER, default 0) saves every 2D texture it samples, each mip level, as
// Documents/TexDump/<fs>-s<slot>-t<serial>-f<format>-l<level>.png (round155b: terrain masks that turn far hills blue).
int wonder_host_readback_png(void *texture, int level, int slice, const char *path_c);
static int readback_alpha;   // defined with the readback below
static void tex_dump(const struct wonder_host_draw *d, const struct wonder_shader_refl *r, __strong id<MTLTexture> *textures) {
    static const char *list = (const char *)1; static NSMutableSet<NSString *> *done;
    if (list == (const char *)1) list = getenv("WONDER_TEX_DUMP_FS");
    if (!list || !*list) return;
    static int after = -1; if (after < 0) { const char *e = getenv("WONDER_TEX_DUMP_AFTER"); after = e ? atoi(e) : 0; }
    if (d->frame < (unsigned)after) return;
    // Entries are a fragment key prefix, or "<vs prefix>-<fs prefix>" for one program (round170: prepass vs 1451a723).
    int match = 0; for (const char *p = list; *p && !match; ) {
        size_t n = strcspn(p, ","); const char *dash = memchr(p, '-', n);
        if (dash) { size_t nv = (size_t)(dash - p), nf = n - nv - 1;
            match = nv && nf && nv <= 16 && nf <= 16 && !strncmp(p, d->vs_key, nv) && !strncmp(dash + 1, d->fs_key, nf); }
        else match = n && n <= strlen(d->fs_key) && !strncmp(p, d->fs_key, n);
        p += n; if (*p == ',') p++;
    }
    if (!match) return;
    if (!done) done = [NSMutableSet set];
    NSString *key = [NSString stringWithFormat:@"%s-%s", d->vs_key, d->fs_key]; if ([done containsObject:key]) return; [done addObject:key];
    NSString *dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents/TexDump"];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    {   // The draw's data too: each uniform buffer both stages read (vec4 rows) and the first vertices' raw attribute words.
        NSMutableString *text = [NSMutableString stringWithFormat:@"vs %s fs %s frame %u count %d\n", d->vs_key, d->fs_key, d->frame, d->count];
        const struct wonder_shader_refl *rf[2] = {d->vr, d->fr};
        for (int st = 0; st < 2; st++) for (int i = 0; i < rf[st]->ncbuf; i++) {
            int slot = rf[st]->cbuf[i].slot; const struct wonder_host_span *u = &d->ubo[st][slot]; if (!u->buffer) continue;
            const float *f = (const float *)((const uint8_t *)((__bridge id<MTLBuffer>)u->buffer).contents + u->offset);
            [text appendFormat:@"ubo stage %d slot %d size %zu\n", st, slot, u->size];
            for (size_t r = 0; r < u->size / 16 && r < 128; r++) [text appendFormat:@"  [%zu] %g %g %g %g\n", r, f[4 * r], f[4 * r + 1], f[4 * r + 2], f[4 * r + 3]];
        }
        for (int a = 0; a < d->nattr; a++) {
            [text appendFormat:@"attr %d (location %d) format %#x offset %u stream %u\n", a, d->vr->attr[a].location, d->attr[a].format, d->attr[a].offset, d->attr[a].stream];
            if (d->attr[a].stream >= 16 || !(d->stream_mask >> d->attr[a].stream & 1)) continue;
            const struct wonder_host_span *sp = &d->stream[d->attr[a].stream].span; uint32_t stride = d->stream[d->attr[a].stream].stride;
            const uint8_t *base = (const uint8_t *)((__bridge id<MTLBuffer>)sp->buffer).contents + sp->offset;
            for (uint32_t v = 0; v < 8 && (uint64_t)v * stride + d->attr[a].offset + 16 <= sp->size; v++) {
                const uint32_t *w = (const uint32_t *)(base + (size_t)v * stride + d->attr[a].offset);
                [text appendFormat:@"  v%u %08x %08x %08x %08x\n", v, w[0], w[1], w[2], w[3]];
            }
        }
        NSString *name = [NSString stringWithFormat:@"%.8s-%.8s-data.txt", d->vs_key, d->fs_key];
        [text writeToFile:[dir stringByAppendingPathComponent:name] atomically:YES encoding:NSUTF8StringEncoding error:nil];
        fprintf(stderr, "HOST_TEX_DUMP Documents/TexDump/%s data\n", name.UTF8String);
    }
    for (int i = 0; i < r->ntex; i++) {
        id<MTLTexture> t = textures[i]; int slot = r->tex[i].slot;
        if (!t || t.textureType != MTLTextureType2D || d->tex[1][slot].empty) continue;
        for (NSUInteger level = 0; level < t.mipmapLevelCount && level < 13; level++) {
            NSString *name = [NSString stringWithFormat:@"%.8s-s%d-t%llu-f%x-l%lu.png", d->fs_key, slot, (unsigned long long)d->tex[1][slot].texture.texture.serial,
                              d->tex[1][slot].texture.texture.desc.format, (unsigned long)level];
            int rc = wonder_host_readback_png((__bridge void *)t, (int)level, 0, [dir stringByAppendingPathComponent:name].fileSystemRepresentation);
            fprintf(stderr, "HOST_TEX_DUMP Documents/TexDump/%s %lux%lu result=%d\n", name.UTF8String, (unsigned long)MAX(1u, t.width >> level), (unsigned long)MAX(1u, t.height >> level), rc);
            NSString *aname = [name stringByReplacingOccurrencesOfString:@".png" withString:@"-alpha.png"];
            readback_alpha = 1; rc = wonder_host_readback_png((__bridge void *)t, (int)level, 0, [dir stringByAppendingPathComponent:aname].fileSystemRepresentation); readback_alpha = 0;
            fprintf(stderr, "HOST_TEX_DUMP Documents/TexDump/%s alpha result=%d\n", aname.UTF8String, rc);
        }
    }
}
// Diagnostic lists of fragment key prefixes ("a,b,c") in an environment variable; logs the first match per variable.
static int diag_fs_listed(const struct wonder_host_draw *d, const char *var) {
    const char *list = getenv(var); if (!list || !*list) return 0;
    for (const char *p = list; *p; ) {
        size_t n = strcspn(p, ",");
        if (n && n <= strlen(d->fs_key) && !strncmp(p, d->fs_key, n)) {
            static NSMutableSet *seen; if (!seen) seen = [NSMutableSet set];
            NSString *k = [NSString stringWithFormat:@"%s:%s", var, d->fs_key];
            if (![seen containsObject:k]) { [seen addObject:k]; fprintf(stderr, "HOST_DIAG %s applies to fs=%s vs=%s\n", var, d->fs_key, d->vs_key); }
            return 1;
        }
        p += n; if (*p == ',') p++;
    }
    return 0;
}
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
    int uses_default = 0;
    for (int i = 0; i < d->nattr; i++) {
        MTLVertexAttributeDescriptor *a = vd.attributes[d->vr->attr[i].location];
        if (d->attr[i].stream == WONDER_HOST_DEFAULT_ATTRIBUTE) {   // one zero float, expanded by Metal to (0,0,0,1)
            a.format = MTLVertexFormatFloat; a.offset = 0; a.bufferIndex = WONDER_SHADER_DEFAULT_ATTRIBUTE_BUFFER; uses_default = 1; continue;
        }
        uint32_t f = wonder_host_vertex_format((int)d->attr[i].format);
        if (!f) unsupported(d, "vertex format", (long)d->attr[i].format);
        a.format = (MTLVertexFormat)f; a.offset = d->attr[i].offset; a.bufferIndex = WONDER_SHADER_STREAM_BUFFER_BASE + d->attr[i].stream;
    }
    if (uses_default) { MTLVertexBufferLayoutDescriptor *l = vd.layouts[WONDER_SHADER_DEFAULT_ATTRIBUTE_BUFFER];
                        l.stepFunction = MTLVertexStepFunctionConstant; l.stepRate = 0; l.stride = 16; }
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
    for (int i = 0; i < d->nattr; i++) { pk.attr[i][0] = d->attr[i].format; pk.attr[i][1] = d->attr[i].offset; pk.attr[i][2] = d->attr[i].stream | (uint32_t)d->vr->attr[i].location << 8; }   // DEFAULT stream 0xff keyed too
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
    {   // Diagnostic (WONDER_DEPTH_EQUAL_LEQUAL=1): EQUAL without depth writes tested as LEQUAL (round164: are D24 depth
        // pre-pass + EQUAL colour passes failing on Depth32Float?)
        static int on = -1; if (on < 0) { const char *e = getenv("WONDER_DEPTH_EQUAL_LEQUAL"); on = e && *e == '1'; }
        if (on && dd.depthCompareFunction == MTLCompareFunctionEqual && !dd.depthWriteEnabled) dd.depthCompareFunction = MTLCompareFunctionLessEqual;
        // WONDER_DEPTH_EQUAL_AS=always|gequal (round188): the course hedges' colour pass runs on the CPU side
        // (main+0x1e555a0 -> 0xe5fe0 returns true, 0x606fc issues it) but with a vertex shader other than the pre-pass
        // 1451a723; does its EQUAL test fail because the two translated shaders' positions differ in the last bit?
        static int as = -1;
        if (as < 0) { const char *e = getenv("WONDER_DEPTH_EQUAL_AS"); as = !e ? 0 : !strcmp(e, "always") ? 1 : !strcmp(e, "gequal") ? 2 : 0; }
        if (as && dd.depthCompareFunction == MTLCompareFunctionEqual && !dd.depthWriteEnabled)
            dd.depthCompareFunction = as == 1 ? MTLCompareFunctionAlways : MTLCompareFunctionGreaterEqual;
    }
    if (diag_fs_listed(d, "WONDER_DIAG_NODEPTH_FS")) dd.depthCompareFunction = MTLCompareFunctionAlways;   // diagnostic: depth test off for listed fragment shaders
    NSData *ds_key = [NSData dataWithBytes:(uint32_t[]){(uint32_t)dd.depthCompareFunction, dd.depthWriteEnabled} length:8];
    id<MTLDepthStencilState> dss = depth_states[ds_key];
    if (!dss) { dss = [device newDepthStencilStateWithDescriptor:dd]; depth_states[ds_key] = dss; }
    // Resolve every resource before taking the replay lock: texture lookups and render-target creation take the same
    // (non-recursive) lock in host_graphics.m (round55 deadlocked at the first draw).
    const struct wonder_shader_refl *refl[2] = {d->vr, d->fr};
    id<MTLTexture> textures[2][40]; id<MTLSamplerState> sampler_states[2][40]; id<MTLBuffer> c1_buffers[2] = {nil, nil}, drv0_buffers[2] = {nil, nil};
    for (int stage = 0; stage < 2; stage++) {
        const struct wonder_shader_refl *r = refl[stage];
        for (int i = 0; i < r->ntex; i++) {
            int slot = r->tex[i].slot;
            if (d->tex[stage][slot].empty) { textures[stage][i] = empty_texture(d, r->tex[i].type); sampler_states[stage][i] = empty_sampler(); continue; }
            textures[stage][i] = bisect_dummy_textures ? dummy_texture() : sampled_texture(d, stage, slot); sampler_states[stage][i] = sampler_state(d, &d->tex[stage][slot].sampler);
            // Maxwell samples a 2D texture through an array instruction as layer 0 (round127: the font shader fs 929a2e26
            // declares texture2d_array, the glyph cache is 2D); Metal needs the declared type, so bind a 1-layer 2D-array view.
            if (r->tex[i].type == WONDER_SHADER_TEX_2D_ARRAY && textures[stage][i].textureType == MTLTextureType2D) {
                id<MTLTexture> t2 = textures[stage][i];
                textures[stage][i] = [t2 newTextureViewWithPixelFormat:t2.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(0, t2.mipmapLevelCount) slices:NSMakeRange(0, 1)];
                if (!textures[stage][i]) unsupported(d, "2D texture as a 2D-array view, serial", (long)d->tex[stage][slot].texture.texture.serial);
            }
            if (r->tex[i].type == WONDER_SHADER_TEX_2D && textures[stage][i].textureType == MTLTextureType2DArray) {   // first layer (round153)
                id<MTLTexture> t2 = textures[stage][i];
                textures[stage][i] = [t2 newTextureViewWithPixelFormat:t2.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, t2.mipmapLevelCount) slices:NSMakeRange(0, 1)];
                if (!textures[stage][i]) unsupported(d, "first layer of a 2D array as a 2D view, serial", (long)d->tex[stage][slot].texture.texture.serial);
            }
        }
        if (stage == 1) tex_dump(d, r, textures[1]);
        if (r->c1_buffer >= 0) {   // the shader's own constants; padded to the 64 KiB a constant buffer may declare
            c1_buffers[stage] = [device newBufferWithLength:0x10000 options:MTLResourceStorageModeShared]; memcpy(c1_buffers[stage].contents, r->c1, r->c1_len);
        }
        if (r->drv0_buffer >= 0) { drv0_buffers[stage] = [device newBufferWithLength:0x10000 options:MTLResourceStorageModeShared]; memcpy(drv0_buffers[stage].contents, d->drv0, sizeof d->drv0); }
        if (r->support >= 0 && !support_buffer) {   // Ryujinx support buffer (layout per D2R nvn_mtl.m): render_scale[0..72].x = 1, everything else 0
            support_buffer = [device newBufferWithLength:0x10000 options:MTLResourceStorageModeShared];
            for (int k = 0; k < 73; k++) *(float *)((uint8_t *)support_buffer.contents + 192 + 16 * k) = 1.0f;
        }
    }
    // Encode: appended to the open pass batch (draw_batch_*), encoded at the next flush.
    MTLCullMode cull = MTLCullModeNone;
    if (d->states_set & 8) { if (d->polygon.cull == 3) unsupported(d, "cull FRONT_AND_BACK", 3);
                             cull = d->polygon.cull == 1 ? MTLCullModeFront : d->polygon.cull == 2 ? MTLCullModeBack : MTLCullModeNone; }
    // NVN front face (CW 0 / CCW 1) is defined on clip-space winding; with UPPER_LEFT origin Metal's framebuffer winding
    // matches the GL clip-control convention, so the NVN value maps directly (pinned by the back-face GPU test).
    MTLWinding winding = (d->states_set & 8) && d->polygon.front == 0 ? MTLWindingClockwise : MTLWindingCounterClockwise;
    MTLViewport vp = {0, 0, (double)width, (double)height, d->depth_range[0], d->depth_range[1]};
    if (d->viewport_set) { vp.originX = d->viewport[0]; vp.originY = d->viewport[1]; vp.width = d->viewport[2]; vp.height = d->viewport[3]; }
    MTLScissorRect scissor = {0, 0, width, height};
    if (d->scissor_set) {
        long x = MAX(0, d->scissor[0]), y = MAX(0, d->scissor[1]);
        long x1 = MIN((long)width, (long)d->scissor[0] + d->scissor[2]), y1 = MIN((long)height, (long)d->scissor[1] + d->scissor[3]);
        if (x1 <= x || y1 <= y) return 0;   // empty scissor: nothing is drawn
        scissor = (MTLScissorRect){(NSUInteger)x, (NSUInteger)y, (NSUInteger)(x1 - x), (NSUInteger)(y1 - y)};
    }
    // Translated shaders declare every c[n] as 64 KiB; Maxwell returns 0 past the bound size, Metal would read the next pool
    // bytes (or past the pool's MTLBuffer). Each read uniform buffer is copied on the GPU into a zero-filled 64 KiB window,
    // in command order: the batch's copies run in one blit encoder ahead of its render encoder, and every operation that
    // could change their sources (UpdateUniformBuffer, fills, other encoders, a draw writing storage) flushes the batch first.
    struct draw_copy copies[2 * 14 + 16]; int ncopies = 0;
    id<MTLBuffer> ubo_copies[2][14] = {{nil}}; NSUInteger ubo_offsets[2][14] = {{0}};
    for (int stage = 0; stage < 2; stage++) for (int i = 0; i < refl[stage]->ncbuf; i++) {
        int slot = refl[stage]->cbuf[i].slot; const struct wonder_host_span *u = &d->ubo[stage][slot];
        id<MTLBuffer> src = (__bridge id<MTLBuffer>)u->buffer; NSUInteger n = MIN((NSUInteger)u->size, (NSUInteger)0x10000);
        if (!src || u->offset + n > src.length) unsupported(d, "uniform buffer span outside its pool buffer, slot", slot);
        NSUInteger offset; id<MTLBuffer> window = scratch_window(0x10000, &offset);
        copies[ncopies++] = (struct draw_copy){src, u->offset, window, offset, n, 0x10000};
        ubo_copies[stage][slot] = window; ubo_offsets[stage][slot] = offset;
    }
    // Streams the draw reads past their bound size: zero-filled copies (Maxwell returns zeros there).
    id<MTLBuffer> stream_copies[16] = {nil}; NSUInteger stream_offsets[16] = {0};
    for (int s = 0; s < 16; s++) if ((d->stream_mask >> s & 1) && d->stream[s].fetch_size > d->stream[s].span.size) {
        id<MTLBuffer> src = (__bridge id<MTLBuffer>)d->stream[s].span.buffer;
        if (!src || d->stream[s].span.offset + d->stream[s].span.size > src.length) unsupported(d, "vertex stream span outside its pool buffer, stream", s);
        NSUInteger n = (NSUInteger)((d->stream[s].fetch_size + 15) & ~15ull), offset;
        id<MTLBuffer> window = scratch_window(n, &offset);
        copies[ncopies++] = (struct draw_copy){src, d->stream[s].span.offset, window, offset, d->stream[s].span.size, n};
        stream_copies[s] = window; stream_offsets[s] = offset;
    }
    // Everything the encoder needs, by value: the block runs at flush time, after `d` was reused (blocks cannot capture
    // C arrays, hence the struct and the NSArrays).
    static id<MTLBuffer> zeros; if (uses_default && !zeros) zeros = [device newBufferWithLength:16 options:MTLResourceStorageModeShared];   // zero-filled
    struct draw_encoding x; memset(&x, 0, sizeof x);
    NSMutableArray *bufs[2] = {[NSMutableArray array], [NSMutableArray array]};
    for (int stage = 0; stage < 2; stage++) {
        const struct wonder_shader_refl *r = refl[stage];
        #define BIND(b_, off_, idx_) do { id<MTLBuffer> bb_ = (b_); if (!bb_) unsupported(d, "draw buffer binding without a buffer, index", (long)(idx_)); \
            [bufs[stage] addObject:bb_]; x.offset[stage][x.nb[stage]] = (off_); x.index[stage][x.nb[stage]++] = (idx_); } while (0)
        for (int i = 0; i < r->ncbuf; i++) BIND(ubo_copies[stage][r->cbuf[i].slot], ubo_offsets[stage][r->cbuf[i].slot], r->cbuf[i].buffer);
        for (int i = 0; i < r->nsbuf; i++) { const struct wonder_host_span *sb = &d->sbuf[stage][r->sbuf[i].slot]; BIND((__bridge id<MTLBuffer>)sb->buffer, sb->offset, r->sbuf[i].buffer); }
        if (r->c1_buffer >= 0) BIND(c1_buffers[stage], 0, (NSUInteger)r->c1_buffer);
        if (r->drv0_buffer >= 0) BIND(drv0_buffers[stage], 0, (NSUInteger)r->drv0_buffer);
        if (r->support >= 0) BIND(support_buffer, 0, (NSUInteger)r->support);
        #undef BIND
    }
    NSMutableArray *stage_textures[2] = {[NSMutableArray array], [NSMutableArray array]}, *stage_samplers[2] = {[NSMutableArray array], [NSMutableArray array]};
    for (int stage = 0; stage < 2; stage++) for (int i = 0; i < refl[stage]->ntex; i++) {
        [stage_textures[stage] addObject:textures[stage][i]]; [stage_samplers[stage] addObject:sampler_states[stage][i]];
        x.tex_index[stage][i] = refl[stage]->tex[i].texture; x.smp_index[stage][i] = refl[stage]->tex[i].sampler; x.ntex[stage] = refl[stage]->ntex;
        if (x.smp_index[stage][i] >= 16) {
            if (refl[stage]->smp_overflow < 0 || x.smp_index[stage][i] >= 48) unsupported(d, "sampler binding past 15 without an argument buffer", x.smp_index[stage][i]);
            id<MTLSamplerState> st = sampler_states[stage][i]; x.smp_ids[stage][x.smp_index[stage][i] - 16] = st.gpuResourceID._impl;
        }
    }
    for (int stage = 0; stage < 2; stage++) x.smp_overflow[stage] = refl[stage]->smp_overflow;
    NSMutableArray *streams = [NSMutableArray array];
    for (int s = 0; s < 16; s++) if (d->stream_mask >> s & 1) {
        id<MTLBuffer> b = stream_copies[s] ?: (__bridge id<MTLBuffer>)d->stream[s].span.buffer;
        [streams addObject:b]; x.stream_offset[x.nstreams] = stream_copies[s] ? stream_offsets[s] : d->stream[s].span.offset; x.stream_index[x.nstreams++] = WONDER_SHADER_STREAM_BUFFER_BASE + s;
    }
    id<MTLBuffer> ib = nil; NSUInteger ib_offset = 0; MTLIndexType itype = MTLIndexTypeUInt16;
    if (d->index_type >= 0) {
        ib = (__bridge id<MTLBuffer>)d->index.buffer; ib_offset = d->index.offset;
        if (d->index_type == 0) {   // Metal has no 8-bit indices: widen to 16 bits for this draw
            id<MTLBuffer> wide = [device newBufferWithLength:MAX((NSUInteger)2, (NSUInteger)d->count * 2) options:MTLResourceStorageModeShared];
            const uint8_t *src = (const uint8_t *)ib.contents + ib_offset; uint16_t *dst = wide.contents;
            for (int i = 0; i < d->count; i++) dst[i] = src[i];
            ib = wide; ib_offset = 0;
        } else if (d->index_type == 2) itype = MTLIndexTypeUInt32;
    }
    x.index_type = d->index_type; x.count = d->count; x.first = d->first; x.instances = d->instances; x.base_instance = d->base_instance; x.base_vertex = d->base_vertex;
    memcpy(x.blend_color, d->blend_color, sizeof x.blend_color);
    x.depth_clamp = d->depth_clamp; x.bias = d->depth_bias_enabled; x.bias_units = d->depth_bias; x.bias_slope = d->depth_bias_slope; x.bias_clamp = d->depth_bias_clamp;
    if (diag_fs_listed(d, "WONDER_DIAG_NODEPTH_FS")) cull = MTLCullModeNone;   // diagnostic (with the depth test above)
    if (diag_fs_listed(d, "WONDER_DIAG_NOCULL_FS")) cull = MTLCullModeNone;   // diagnostic (round190): face culling only
    x.cull = cull; x.winding = winding; x.viewport = vp; x.scissor = scissor; x.primitive = primitive; x.itype = itype; x.ib_offset = ib_offset; x.uses_default = uses_default;
    NSString *label = [NSString stringWithFormat:@"NVN draw vs=%s fs=%s", d->vs_key, d->fs_key];
    NSArray *vbufs = bufs[0], *fbufs = bufs[1], *vtex = stage_textures[0], *ftex = stage_textures[1], *vsmp = stage_samplers[0], *fsmp = stage_samplers[1];
    void (^encode)(id<MTLRenderCommandEncoder>) = ^(id<MTLRenderCommandEncoder> e) {
        [e pushDebugGroup:label];
        [e setRenderPipelineState:pso]; [e setDepthStencilState:dss];
        [e setBlendColorRed:x.blend_color[0] green:x.blend_color[1] blue:x.blend_color[2] alpha:x.blend_color[3]];
        [e setCullMode:x.cull]; [e setFrontFacingWinding:x.winding]; [e setDepthClipMode:x.depth_clamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
        [e setDepthBias:x.bias ? x.bias_units : 0 slopeScale:x.bias ? x.bias_slope : 0 clamp:x.bias ? x.bias_clamp : 0];   // state persists across the batch's draws
        [e setViewport:x.viewport]; [e setScissorRect:x.scissor];
        for (int stage = 0; stage < 2; stage++) for (int i = 0; i < x.nb[stage]; i++) {
            if (stage) [e setFragmentBuffer:fbufs[i] offset:x.offset[stage][i] atIndex:x.index[stage][i]];
            else [e setVertexBuffer:vbufs[i] offset:x.offset[stage][i] atIndex:x.index[stage][i]];
        }
        for (int stage = 0; stage < 2; stage++) for (int i = 0; i < x.ntex[stage]; i++) {
            int si = x.smp_index[stage][i];
            if (stage) { [e setFragmentTexture:ftex[i] atIndex:x.tex_index[stage][i]]; if (si < 16) [e setFragmentSamplerState:fsmp[i] atIndex:si]; }
            else { [e setVertexTexture:vtex[i] atIndex:x.tex_index[stage][i]]; if (si < 16) [e setVertexSamplerState:vsmp[i] atIndex:si]; }
        }
        if (x.smp_overflow[1] >= 0) [e setFragmentBytes:x.smp_ids[1] length:sizeof x.smp_ids[1] atIndex:(NSUInteger)x.smp_overflow[1]];
        if (x.smp_overflow[0] >= 0) [e setVertexBytes:x.smp_ids[0] length:sizeof x.smp_ids[0] atIndex:(NSUInteger)x.smp_overflow[0]];
        if (x.uses_default) [e setVertexBuffer:zeros offset:0 atIndex:WONDER_SHADER_DEFAULT_ATTRIBUTE_BUFFER];
        for (int i = 0; i < x.nstreams; i++) [e setVertexBuffer:streams[i] offset:x.stream_offset[i] atIndex:x.stream_index[i]];
        if (x.index_type < 0) [e drawPrimitives:x.primitive vertexStart:(NSUInteger)x.first vertexCount:(NSUInteger)x.count instanceCount:(NSUInteger)x.instances baseInstance:(NSUInteger)x.base_instance];
        else [e drawIndexedPrimitives:x.primitive indexCount:(NSUInteger)x.count indexType:x.itype indexBuffer:ib indexBufferOffset:x.ib_offset
                       instanceCount:(NSUInteger)x.instances baseVertex:x.base_vertex baseInstance:(NSUInteger)x.base_instance];
        [e popDebugGroup];
    };
    // Batching rules: same attachments; no sampled texture is one of the batch's attachments (it must see the earlier
    // draws' output, i.e. a stored pass); a draw writing storage buffers ends its batch (later copies / draws read it).
    int writes = 0;
    for (int stage = 0; stage < 2; stage++) for (int i = 0; i < refl[stage]->nsbuf; i++) writes |= refl[stage]->sbuf[i].write;
    wonder_host_replay_lock();
    if (draw_batch_open() && (!draw_batch_same_pass(pass) || draw_batch_attachment_sampled(textures[0], refl[0]->ntex) || draw_batch_attachment_sampled(textures[1], refl[1]->ntex)))
        batch_close();
    if (!draw_batch_open() && (draw_batch_attachment_sampled_by(pass, textures[0], refl[0]->ntex) || draw_batch_attachment_sampled_by(pass, textures[1], refl[1]->ntex))) {
        // The draw samples one of its own attachments: a pass of its own, as before batching.
        draw_batch_begin(pass); draw_batch_add(copies, ncopies, encode); batch_note_reads(textures[0], refl[0]->ntex); batch_note_reads(textures[1], refl[1]->ntex); batch_close();
    } else {
        if (!draw_batch_open()) draw_batch_begin(pass);
        draw_batch_add(copies, ncopies, encode); batch_note_reads(textures[0], refl[0]->ntex); batch_note_reads(textures[1], refl[1]->ntex);
        if (writes) batch_close();
    }
    wonder_host_replay_unlock();
    if (getenv("WONDER_DRAW_SYNC")) sync_check(d);
    return 0;
}

// ---------- Compute dispatch encoding ----------
// One compute encoder per dispatch, appended to the replay's pending command buffer after the draws before it (Metal
// orders the encoders and tracks hazards on the pool buffers and textures). Bindings follow the kernel's reflection:
// uniform buffers as zero-padded 64 KiB copies (as for draws), uniforms read by address and storage buffers as pool
// spans, textures + samplers, images as GPU textures (a mip level / layer range of the bound view).
static NSMutableDictionary<NSValue *, id<MTLComputePipelineState>> *compute_pipelines;
int wonder_host_dispatch(const struct wonder_host_dispatch *x) {
    const struct wonder_host_draw *d = &x->res; const struct wonder_shader_refl *r = x->r;
    if (!device) wonder_nvn_stop("NVN_DISPATCH_UNSUPPORTED no Metal device (cs=%s)", x->key);
    if (!compute_pipelines) compute_pipelines = [NSMutableDictionary dictionary];
    NSValue *pk = [NSValue valueWithPointer:x->cs];
    id<MTLComputePipelineState> pso = compute_pipelines[pk];
    if (!pso) {
        __block NSError *error = nil; __block id<MTLComputePipelineState> built = nil;
        on_host_stack(^{ NSError *e = nil; built = [device newComputePipelineStateWithFunction:(__bridge id<MTLFunction>)x->cs error:&e]; error = e; }); pso = built;
        if (!pso) wonder_nvn_stop("NVN_DISPATCH_PIPELINE_FAILED cs=%s: %s", x->key, error.localizedDescription.UTF8String);
        compute_pipelines[pk] = pso;
    }
    NSUInteger threads = (NSUInteger)r->local[0] * r->local[1] * r->local[2];
    if (threads > pso.maxTotalThreadsPerThreadgroup)
        wonder_nvn_stop("NVN_DISPATCH_UNSUPPORTED cs=%s threadgroup %u x %u x %u exceeds the pipeline's %lu threads", x->key, r->local[0], r->local[1], r->local[2], (unsigned long)pso.maxTotalThreadsPerThreadgroup);
    // Resolve textures before the replay lock (sampled_texture / render targets take host_graphics.m's lock).
    id<MTLTexture> textures[40]; id<MTLSamplerState> sampler_states[40]; id<MTLTexture> images[8];
    for (int i = 0; i < r->ntex; i++) {
        int slot = r->tex[i].slot;
        if (d->tex[0][slot].empty) { textures[i] = empty_texture(d, r->tex[i].type); sampler_states[i] = empty_sampler(); continue; }
        textures[i] = sampled_texture(d, 0, slot); sampler_states[i] = sampler_state(d, &d->tex[0][slot].sampler);
        if (r->tex[i].type == WONDER_SHADER_TEX_2D_ARRAY && textures[i].textureType == MTLTextureType2D) {
            id<MTLTexture> t2 = textures[i];
            textures[i] = [t2 newTextureViewWithPixelFormat:t2.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(0, t2.mipmapLevelCount) slices:NSMakeRange(0, 1)];
            if (!textures[i]) unsupported(d, "2D texture as a 2D-array view, serial", (long)d->tex[0][slot].texture.texture.serial);
        }
        if (r->tex[i].type == WONDER_SHADER_TEX_2D && textures[i].textureType == MTLTextureType2DArray) {   // first layer (round153)
            id<MTLTexture> t2 = textures[i];
            textures[i] = [t2 newTextureViewWithPixelFormat:t2.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, t2.mipmapLevelCount) slices:NSMakeRange(0, 1)];
            if (!textures[i]) unsupported(d, "first layer of a 2D array as a 2D view, serial", (long)d->tex[0][slot].texture.texture.serial);
        }
    }
    for (int i = 0; i < r->nimg; i++) {
        int slot = r->img[i].slot; const struct wonder_nvn_texture_snapshot *s = &x->img[slot].image;
        id<MTLTexture> t = (__bridge id<MTLTexture>)x->img[slot].host;
        if (!t || !(t.usage & MTLTextureUsageShaderWrite)) unsupported(d, "image texture without shader-write usage, serial", (long)s->texture.serial);
        NSUInteger level = s->has_view && s->view.set_levels ? s->view.base_level : 0;
        NSRange all = wonder_host_view_slices(t.textureType, t.arrayLength, 0, 0), range = all;
        if (s->has_view && s->view.set_layers) range = wonder_host_view_slices(t.textureType, t.arrayLength, (NSUInteger)MAX(0, s->view.min_layer), (NSUInteger)MAX(0, s->view.layers));
        if (level >= t.mipmapLevelCount || !range.length) unsupported(d, "image view outside the texture, serial", (long)s->texture.serial);
        // The kernel declares the image type; a single layer of an array bound to a 2D image is a 2D view of that layer.
        MTLTextureType type = t.textureType;
        if (r->img[i].type == WONDER_SHADER_TEX_2D && (type == MTLTextureType2DArray || type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray)) {
            if (range.length != all.length && range.length != 1) unsupported(d, "2D image over several layers, serial", (long)s->texture.serial);
            if (range.length == all.length && all.length != 1) unsupported(d, "2D image over a layered texture, serial", (long)s->texture.serial);
            type = MTLTextureType2D;
        }
        static const MTLTextureType declared[10] = {MTLTextureType1D, MTLTextureType2D, MTLTextureType3D, MTLTextureType1DArray, MTLTextureType2DArray,
            MTLTextureType2DMultisample, MTLTextureType2DMultisampleArray, MTLTextureTypeCube, MTLTextureTypeCubeArray, MTLTextureTypeTextureBuffer};
        if (r->img[i].type >= 10 || declared[r->img[i].type] != type) unsupported(d, "image type differs from the kernel's declaration, serial", (long)s->texture.serial);
        images[i] = level == 0 && t.mipmapLevelCount == 1 && range.location == 0 && range.length == all.length && type == t.textureType ? t
                  : [t newTextureViewWithPixelFormat:t.pixelFormat textureType:type levels:NSMakeRange(level, 1) slices:range];
        if (!images[i]) unsupported(d, "image view creation, serial", (long)s->texture.serial);
    }
    id<MTLBuffer> c1_buffer = nil, drv0_buffer = nil;
    if (r->c1_buffer >= 0) { c1_buffer = [device newBufferWithLength:0x10000 options:MTLResourceStorageModeShared]; memcpy(c1_buffer.contents, r->c1, r->c1_len); }
    if (r->drv0_buffer >= 0) { drv0_buffer = [device newBufferWithLength:0x10000 options:MTLResourceStorageModeShared]; memcpy(drv0_buffer.contents, d->drv0, sizeof d->drv0); }
    if (r->support >= 0 && !support_buffer) {
        support_buffer = [device newBufferWithLength:0x10000 options:MTLResourceStorageModeShared];
        for (int k = 0; k < 73; k++) *(float *)((uint8_t *)support_buffer.contents + 192 + 16 * k) = 1.0f;
    }
    wonder_host_replay_lock();
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)wonder_host_replay_command_buffer();
    id<MTLBuffer> ubo_copies[14] = {nil};
    if (r->ncbuf) {
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        for (int i = 0; i < r->ncbuf; i++) {
            int slot = r->cbuf[i].slot; const struct wonder_host_span *u = &x->ubo[slot];
            id<MTLBuffer> src = (__bridge id<MTLBuffer>)u->buffer; NSUInteger n = MIN((NSUInteger)u->size, (NSUInteger)0x10000);
            if (!src || u->offset + n > src.length) { [blit endEncoding]; wonder_host_replay_unlock(); unsupported(d, "uniform buffer span outside its pool buffer, compute slot", slot); }
            id<MTLBuffer> copy = [device newBufferWithLength:0x10000 options:MTLResourceStorageModePrivate];
            [blit fillBuffer:copy range:NSMakeRange(0, 0x10000) value:0];
            if (n) [blit copyFromBuffer:src sourceOffset:u->offset toBuffer:copy destinationOffset:0 size:n];
            ubo_copies[slot] = copy;
        }
        [blit endEncoding];
    }
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    if (!e) { wonder_host_replay_unlock(); unsupported(d, "compute encoder creation", 0); }
    e.label = [NSString stringWithFormat:@"NVN dispatch cs=%s", x->key];
    [e setComputePipelineState:pso];
    for (int i = 0; i < r->ncbuf; i++) [e setBuffer:ubo_copies[r->cbuf[i].slot] offset:0 atIndex:r->cbuf[i].buffer];
    for (int i = 0; i < r->nuptr; i++) { const struct wonder_host_span *u = &x->uptr[r->uptr[i].slot]; [e setBuffer:(__bridge id<MTLBuffer>)u->buffer offset:u->offset atIndex:r->uptr[i].buffer]; }
    for (int i = 0; i < r->nsbuf; i++) { const struct wonder_host_span *sb = &x->sbuf[r->sbuf[i].slot]; [e setBuffer:(__bridge id<MTLBuffer>)sb->buffer offset:sb->offset atIndex:r->sbuf[i].buffer]; }
    if (c1_buffer) [e setBuffer:c1_buffer offset:0 atIndex:(NSUInteger)r->c1_buffer];
    if (drv0_buffer) [e setBuffer:drv0_buffer offset:0 atIndex:(NSUInteger)r->drv0_buffer];
    if (r->support >= 0) [e setBuffer:support_buffer offset:0 atIndex:(NSUInteger)r->support];
    for (int i = 0; i < r->ntex; i++) { [e setTexture:textures[i] atIndex:r->tex[i].texture]; [e setSamplerState:sampler_states[i] atIndex:r->tex[i].sampler]; }
    for (int i = 0; i < r->nimg; i++) [e setTexture:images[i] atIndex:r->img[i].texture];
    MTLSize local = MTLSizeMake(r->local[0], r->local[1], r->local[2]);
    if (x->indirect) [e dispatchThreadgroupsWithIndirectBuffer:(__bridge id<MTLBuffer>)x->indirect_span.buffer indirectBufferOffset:x->indirect_span.offset threadsPerThreadgroup:local];
    else [e dispatchThreadgroups:MTLSizeMake((NSUInteger)x->groups[0], (NSUInteger)x->groups[1], (NSUInteger)x->groups[2]) threadsPerThreadgroup:local];
    [e endEncoding];
    wonder_host_replay_unlock();
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
// Metal texture-view type rules: 2D -> 2D / 2D array; 2D array, cube, cube array -> any of those four; 3D -> 3D only.
long wonder_host_view_type(MTLTextureType source, int nvn_target) {
    long view; switch (nvn_target) {
    case 1: case 7: view = MTLTextureType2D; break;
    case 4: view = MTLTextureType2DArray; break;
    case 8: view = MTLTextureTypeCube; break;
    case 9: view = MTLTextureTypeCubeArray; break;
    case 2: view = MTLTextureType3D; break;
    default: return -1;
    }
    int layered = source == MTLTextureType2DArray || source == MTLTextureTypeCube || source == MTLTextureTypeCubeArray;
    if (source == MTLTextureType2D) return view == MTLTextureType2D || view == MTLTextureType2DArray ? view : -1;
    if (layered) return view != MTLTextureType3D ? view : -1;
    return (long)source == view ? view : -1;
}
NSRange wonder_host_view_type_slices(MTLTextureType source, NSUInteger array_length, MTLTextureType view, NSUInteger first, NSUInteger count) {
    NSUInteger total = wonder_host_view_slices(source, array_length, 0, 0).length;
    if (view == source) return wonder_host_view_slices(source, array_length, first, count);
    NSRange r;
    if (view == MTLTextureTypeCube || view == MTLTextureTypeCubeArray) {   // whole cubes out of the source's layer-faces
        if (total % 6) return NSMakeRange(0, 0);
        r = wonder_host_view_slices(MTLTextureTypeCubeArray, total / 6, first, count);
        if (view == MTLTextureTypeCube && r.length != 6) return NSMakeRange(0, 0);
        return r;
    }
    r = wonder_host_view_slices(MTLTextureType2DArray, total, first, count);   // NVN layer-face index = Metal slice
    if (view == MTLTextureType2D && r.length != 1) return NSMakeRange(0, 0);
    return r;
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
    if (t->flags & 0x4) d.usage |= MTLTextureUsageShaderWrite;   // NVN_TEXTURE_FLAGS_IMAGE: compute kernels store into it
    return d;
}
// ---------- Scissored / masked colour clears ----------
static NSString *const clear_source = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; };\n"
    "vertex V clear_vs(uint id [[vertex_id]]) { V o; o.p = float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, 0.0, 1.0); return o; }\n"
    "fragment float4 clear_fs(constant float4 &c [[buffer(0)]]) { return c; }\n";
// nvnCommandBufferCopyTextureToTexture between GPU-resident textures (round132, in a course): same-size regions as a
// Metal blit, layer by layer. A CPU-only source or a scaled copy is refused by name in *why.
int wonder_host_copy_texture(const struct wonder_nvn_texture_snapshot *s, const struct wonder_nvn_copy_region *sr,
                             const struct wonder_nvn_texture_snapshot *d, const struct wonder_nvn_copy_region *dr, int flags, char *why, size_t n) {
    id<MTLTexture> st = (__bridge id<MTLTexture>)wonder_host_render_target_existing(&s->texture);
    if (!st) { snprintf(why, n, "copy source serial %llu is not GPU-resident (CPU texture %ux%u fmt %#x)", (unsigned long long)s->texture.serial, s->texture.desc.width, s->texture.desc.height, s->texture.desc.format); return -1; }
    uint32_t mtl = 0; if (nvn_fmt_to_mtl(d->texture.desc.format, &mtl, NULL) != 1) { snprintf(why, n, "copy destination format %#x", d->texture.desc.format); return -1; }
    id<MTLTexture> dt = (__bridge id<MTLTexture>)wonder_host_render_target(&d->texture, mtl);
    if (!dt) { snprintf(why, n, "copy destination serial %llu has no GPU texture", (unsigned long long)d->texture.serial); return -1; }
    if (sr->width != dr->width || sr->height != dr->height || sr->depth != dr->depth) {
        snprintf(why, n, "scaled copy %dx%dx%d -> %dx%dx%d flags %#x", sr->width, sr->height, sr->depth, dr->width, dr->height, dr->depth, flags); return -1; }
    if (st.pixelFormat != dt.pixelFormat) { snprintf(why, n, "copy between formats %lu -> %lu", (unsigned long)st.pixelFormat, (unsigned long)dt.pixelFormat); return -1; }
    if (sr->width <= 0 || sr->height <= 0 || sr->depth <= 0 || sr->x < 0 || sr->y < 0 || dr->x < 0 || dr->y < 0) { snprintf(why, n, "copy region %d,%d %dx%d", sr->x, sr->y, sr->width, sr->height); return -1; }
    NSUInteger sl = s->has_view && s->view.set_levels ? s->view.base_level : 0, dl = d->has_view && d->view.set_levels ? d->view.base_level : 0;
    NSUInteger ss = (s->has_view && s->view.set_layers ? s->view.min_layer : 0) + (NSUInteger)sr->z, ds = (d->has_view && d->view.set_layers ? d->view.min_layer : 0) + (NSUInteger)dr->z;
    int is3d = st.textureType == MTLTextureType3D;
    wonder_host_replay_lock();
    id<MTLBlitCommandEncoder> b = [(__bridge id<MTLCommandBuffer>)wonder_host_replay_command_buffer() blitCommandEncoder];
    for (int z = 0; z < (is3d ? 1 : sr->depth); z++)
        [b copyFromTexture:st sourceSlice:ss + (NSUInteger)z sourceLevel:sl sourceOrigin:MTLOriginMake((NSUInteger)sr->x, (NSUInteger)sr->y, is3d ? (NSUInteger)sr->z : 0)
                sourceSize:MTLSizeMake((NSUInteger)sr->width, (NSUInteger)sr->height, is3d ? (NSUInteger)sr->depth : 1)
                 toTexture:dt destinationSlice:ds + (NSUInteger)z destinationLevel:dl destinationOrigin:MTLOriginMake((NSUInteger)dr->x, (NSUInteger)dr->y, is3d ? (NSUInteger)dr->z : 0)];
    [b endEncoding];
    wonder_host_replay_unlock();
    return 0;
}
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
int wonder_host_readback_png(void *texture, int level, int slice, const char *path_c) { return wonder_host_readback_png_rect(texture, level, slice, NULL, path_c); }
// rect {x, y, w, h} within the level (NULL = the whole level): presents are saved as their crop rectangle, as displayed.
// Other colour / depth formats (diagnostic frame dumps, round153: R11G11B10F, RGBA16F, R16F, R8, depth): the level /
// slice is drawn into an RGBA8 texture first (colour clamped to 0..1, alpha 1; depth as grey). Integer formats: 2.
static NSString *const readback_source = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; float2 uv; };\n"
    "vertex V rb_vs(uint id [[vertex_id]]) { float2 c = float2(id & 1, id >> 1); V o; o.p = float4(c.x * 2 - 1, 1 - c.y * 2, 0, 1); o.uv = c; return o; }\n"
    "constexpr sampler rb_s(filter::nearest, coord::normalized, address::clamp_to_edge);\n"
    "fragment float4 rb_color(V i [[stage_in]], texture2d<float> t [[texture(0)]]) { return float4(saturate(t.sample(rb_s, i.uv).rgb), 1); }\n"
    "fragment float4 rb_alpha(V i [[stage_in]], texture2d<float> t [[texture(0)]]) { float a = saturate(t.sample(rb_s, i.uv).a); return float4(a, a, a, 1); }\n"
    "fragment float4 rb_depth(V i [[stage_in]], depth2d<float> t [[texture(0)]]) { uint2 q = uint2(i.uv * float2(t.get_width(), t.get_height())); float d = t.read(q); return float4(d, d, d, 1); }\n";
static int readback_alpha;   // tex_dump: the next conversions draw the alpha channel as grey (tentative definition)
static id<MTLTexture> readback_convert(id<MTLTexture> t, int level, int slice) {
    static id<MTLRenderPipelineState> color, depth, alpha;
    if (!alpha) {
        NSError *error = nil; id<MTLLibrary> lib = [device newLibraryWithSource:readback_source options:nil error:&error]; if (!lib) return nil;
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new]; d.vertexFunction = [lib newFunctionWithName:@"rb_vs"];
        d.fragmentFunction = [lib newFunctionWithName:@"rb_alpha"]; d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        alpha = [device newRenderPipelineStateWithDescriptor:d error:&error]; if (!alpha) return nil;
    }
    if (!color) {
        NSError *error = nil; id<MTLLibrary> lib = [device newLibraryWithSource:readback_source options:nil error:&error]; if (!lib) return nil;
        for (int k = 0; k < 2; k++) {
            MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new]; d.vertexFunction = [lib newFunctionWithName:@"rb_vs"];
            d.fragmentFunction = [lib newFunctionWithName:k ? @"rb_depth" : @"rb_color"]; d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            id<MTLRenderPipelineState> p = [device newRenderPipelineStateWithDescriptor:d error:&error]; if (!p) return nil;
            if (k) depth = p; else color = p;
        }
    }
    MTLPixelFormat f = t.pixelFormat;
    int is_depth = f == MTLPixelFormatDepth32Float || f == MTLPixelFormatDepth16Unorm || f == MTLPixelFormatDepth32Float_Stencil8;   // (Depth24Unorm_Stencil8 does not exist on iOS)
    if (!is_depth && (f == MTLPixelFormatStencil8 || f == MTLPixelFormatX32_Stencil8)) return nil;
    id<MTLTexture> view = [t newTextureViewWithPixelFormat:f textureType:MTLTextureType2D levels:NSMakeRange((NSUInteger)level, 1) slices:NSMakeRange((NSUInteger)slice, 1)];
    if (!view) return nil;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:view.width height:view.height mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> out = [device newTextureWithDescriptor:td]; if (!out) return nil;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = out; pass.colorAttachments[0].loadAction = MTLLoadActionDontCare; pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)wonder_host_replay_command_buffer();
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:is_depth ? depth : readback_alpha ? alpha : color]; [e setFragmentTexture:view atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4]; [e endEncoding];
    return out;
}
static int integer_format(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint: case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint: case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint:
    case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint: case MTLPixelFormatRG16Uint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint:
    case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint: case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint:
    case MTLPixelFormatRGB10A2Uint: return 1;
    default: return 0;
    }
}
int wonder_host_readback_png_rect(void *texture, int level, int slice, const int rect[4], const char *path_c) {
    id<MTLTexture> t = (__bridge id<MTLTexture>)texture;
    if (!t || !device || !path_c) return 1;
    MTLPixelFormat f = t.pixelFormat;
    int bgra = f == MTLPixelFormatBGRA8Unorm || f == MTLPixelFormatBGRA8Unorm_sRGB, converted = 0;
    if (readback_alpha || (!bgra && f != MTLPixelFormatRGBA8Unorm && f != MTLPixelFormatRGBA8Unorm_sRGB)) {
        if (integer_format(f) || f == MTLPixelFormatStencil8) return 2;
        converted = 1;
    }
    NSUInteger lw = MAX((NSUInteger)1, t.width >> level), lh = MAX((NSUInteger)1, t.height >> level), x0 = 0, y0 = 0, w = lw, h = lh;
    if (rect) {
        if (rect[0] < 0 || rect[1] < 0 || rect[2] <= 0 || rect[3] <= 0 || (NSUInteger)rect[0] + (NSUInteger)rect[2] > lw || (NSUInteger)rect[1] + (NSUInteger)rect[3] > lh) return 3;
        x0 = (NSUInteger)rect[0]; y0 = (NSUInteger)rect[1]; w = (NSUInteger)rect[2]; h = (NSUInteger)rect[3];
    }
    NSUInteger pitch = w * 4;
    id<MTLBuffer> out = [device newBufferWithLength:pitch * h options:MTLResourceStorageModeShared];
    NSString *path = [NSString stringWithUTF8String:path_c];
    wonder_host_replay_lock();
    if (converted) {
        t = readback_convert(t, level, slice);
        if (!t) { wonder_host_replay_unlock(); return 2; }
        level = 0; slice = 0;
    }
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)wonder_host_replay_command_buffer();
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:MTLOriginMake(x0, y0, 0) sourceSize:MTLSizeMake(w, h, 1)
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
