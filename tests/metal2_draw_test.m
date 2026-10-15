// Shader library on a real Metal device: manifest-verified MSL compiled as MSL 2.1, cached by key; every failure stops
// by name. argv[1] = scratch dir; argv[2] = "ok" (positive checks) or a stop case run in its own process (Metal is not
// fork-safe); argv[3] (optional, with "ok") = work/build/wonder-shaders to compile the real translated stages.
#import <Metal/Metal.h>
#include "metal2_draw.h"
#include "host_draw.h"
#include <CommonCrypto/CommonDigest.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
_Noreturn void wonder_nvn_stop(const char *format, ...) { va_list a; va_start(a, format); vfprintf(stderr, format, a); va_end(a); fputc('\n', stderr); exit(3); }
// Replay hooks (host_graphics.m on device); the shader-library checks never draw.
void wonder_host_gpu_error_reset(void) {} void wonder_host_finish(void) {} int wonder_host_gpu_error(char *out, size_t size) { (void)out; (void)size; return 0; }
void wonder_host_replay_lock(void) {} void wonder_host_replay_unlock(void) {}
void *wonder_host_replay_command_buffer(void) { return NULL; }
void *wonder_host_render_target_existing(const struct wonder_nvn_texture *t) { (void)t; return NULL; }
void *wonder_host_render_target(const struct wonder_nvn_texture *t, uint32_t format) { (void)t; (void)format; return NULL; }
static NSString *dir;
static NSString *hex(NSData *d) { unsigned char h[32]; CC_SHA256(d.bytes, (CC_LONG)d.length, h); NSMutableString *s = [NSMutableString string]; for (int i = 0; i < 32; i++) [s appendFormat:@"%02x", h[i]]; return s; }
static void put(NSString *key, NSString *stage, NSString *msl, NSString *refl, NSMutableDictionary *keys) {
    NSData *m = [msl dataUsingEncoding:NSUTF8StringEncoding], *r = [refl dataUsingEncoding:NSUTF8StringEncoding];
    [m writeToFile:[dir stringByAppendingFormat:@"/%@.metal", key] atomically:YES]; [r writeToFile:[dir stringByAppendingFormat:@"/%@.refl", key] atomically:YES];
    keys[key] = @{@"stage": stage, @"msl_sha256": hex(m), @"refl_sha256": hex(r)};
}
static void missing(void) { wonder_host_shader_function("0000000000000000", 0); }
static void broken(void) { wonder_host_shader_function("bbbbbbbbbbbbbbbb", 1); }
static void tampered(void) { wonder_host_shader_function("cccccccccccccccc", 0); }
static void wrong_stage(void) { wonder_host_shader_function("aaaaaaaaaaaaaaaa", 1); }
static void positive(int argc, char **argv) {
    assert(wonder_host_shader_available("aaaaaaaaaaaaaaaa") == 1 && wonder_host_shader_available("0000000000000000") == 0);
    void *f = wonder_host_shader_function("aaaaaaaaaaaaaaaa", 0);
    assert(f && ((__bridge id<MTLFunction>)f).functionType == MTLFunctionTypeVertex);
    assert(wonder_host_shader_function("aaaaaaaaaaaaaaaa", 0) == f);                     // cached by key
    const struct wonder_shader_refl *r = wonder_host_shader_refl("aaaaaaaaaaaaaaaa"); assert(r && r->stage == 0);
    if (argc > 3) {   // real translated stages from tools/wonder_shader_build.py
        wonder_host_shader_set_directory(argv[3]);
        NSDictionary *m = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[NSString stringWithFormat:@"%s/manifest.json", argv[3]]] options:0 error:nil];
        for (NSString *key in m[@"keys"]) {
            int stage = [m[@"keys"][key][@"stage"] isEqual:@"ps"];
            id<MTLFunction> fn = (__bridge id<MTLFunction>)wonder_host_shader_function(key.UTF8String, stage);
            assert(fn.functionType == (stage ? MTLFunctionTypeFragment : MTLFunctionTypeVertex));
            assert(wonder_host_shader_refl(key.UTF8String)->stage == stage);
        }
    }
}
int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice(); if (!device) return 77;
        if (argc < 3) return 2;
        dir = [NSString stringWithUTF8String:argv[1]];
        NSString *vs = @"#include <metal_stdlib>\nusing namespace metal;\nstruct o { float4 p [[position]]; };\nvertex o main0() { o r; r.p = float4(0, 0, 0, 1); return r; }\n";
        NSMutableDictionary *keys = [NSMutableDictionary dictionary];
        put(@"aaaaaaaaaaaaaaaa", @"vs", vs, @"stage vs\n", keys);
        put(@"bbbbbbbbbbbbbbbb", @"ps", @"fragment float4 main0( {", @"stage ps\nfout 0 float\n", keys);
        put(@"cccccccccccccccc", @"vs", vs, @"stage vs\n", keys);
        [@"// edited after the build" writeToFile:[dir stringByAppendingString:@"/cccccccccccccccc.metal"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
        [[NSJSONSerialization dataWithJSONObject:@{@"keys": keys} options:0 error:nil] writeToFile:[dir stringByAppendingString:@"/manifest.json"] atomically:YES];
        wonder_host_draw_set_device((__bridge void *)device);
        wonder_host_shader_set_directory(argv[1]);
        const char *mode = argv[2];
        if (!strcmp(mode, "ok")) { positive(argc, argv); puts("metal2_draw shader library OK"); return 0; }
        if (!strcmp(mode, "missing")) missing();
        if (!strcmp(mode, "broken")) broken();
        if (!strcmp(mode, "tampered")) tampered();
        if (!strcmp(mode, "wrong_stage")) wrong_stage();
        return 4;   // a stop case that did not stop
    }
}
