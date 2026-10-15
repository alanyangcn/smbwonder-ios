#import <Metal/Metal.h>
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
// Metal encoding of resolved draws lands with the PSO/texture work; until then a resolved draw stops by name.
int wonder_host_draw(const struct wonder_host_draw *d) {
    wonder_nvn_stop("NVN_HOST_DRAW_PENDING vs=%s fs=%s: Metal draw encoding not implemented yet", d->vs_key, d->fs_key);
}
