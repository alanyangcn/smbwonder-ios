// wonder_host_draw on a real Metal device: one resolved draw with a block-linear texture, a uniform buffer, c1 constants,
// u8 indices, back-face culling (NVN CCW front) and a LESS depth test; colour and depth are read back.
// argv[1] = "draw" | "bad_vertex_format" | "back_facing" (each run in its own process: Metal is not fork-safe).
#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
#include <unistd.h>
#include "metal2_draw.h"
#include "host_draw.h"
#include "nvn_deswizzle.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
_Noreturn void wonder_nvn_stop(const char *format, ...) { va_list a; va_start(a, format); vfprintf(stderr, format, a); va_end(a); fputc('\n', stderr); exit(3); }
// Host replay hooks normally supplied by host_graphics.m.
static id<MTLDevice> device; static id<MTLCommandBuffer> pending; static id<MTLCommandQueue> queue;
static NSMutableDictionary<NSNumber *, id<MTLTexture>> *targets;
void wonder_host_replay_lock(void) {} void wonder_host_replay_unlock(void) {}
void *wonder_host_replay_command_buffer(void) { if (!pending) pending = [queue commandBuffer]; return (__bridge void *)pending; }
void *wonder_host_render_target_existing(const struct wonder_nvn_texture *t) { return (__bridge void *)targets[@(t->serial)]; }
void *wonder_host_render_target(const struct wonder_nvn_texture *t, uint32_t format) {
    id<MTLTexture> x = targets[@(t->serial)];
    if (!x) { MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)format width:t->desc.width height:t->desc.height mipmapped:NO];
              d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate; x = [device newTextureWithDescriptor:d]; targets[@(t->serial)] = x; }
    return (__bridge void *)x;
}
static NSString *const vs_src = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "struct I { float4 p [[attribute(0)]]; };\nstruct O { float4 p [[position]]; };\nstruct C1 { float4 v[4]; };\n"
    "vertex O main0(I in [[stage_in]], constant C1& c1 [[buffer(2)]]) { O o; o.p = in.p + c1.v[0]; return o; }\n";
static NSString *const fs_src = @"#include <metal_stdlib>\nusing namespace metal;\nstruct U { float4 tint; };\n"
    "fragment float4 main0(constant U& u [[buffer(1)]], texture2d<float> t [[texture(1)]], sampler s [[sampler(1)]]) { return t.sample(s, float2(0.25, 0.75)) * u.tint; }\n";
static id<MTLFunction> function(NSString *source) {
    NSError *e = nil; MTLCompileOptions *o = [MTLCompileOptions new]; o.languageVersion = MTLLanguageVersion2_1;
    id<MTLLibrary> l = [device newLibraryWithSource:source options:o error:&e]; assert(l); return [l newFunctionWithName:@"main0"];
}
static struct wonder_nvn_texture target_texture(uint64_t serial, int format) {
    struct wonder_nvn_texture t = {0}; t.serial = serial; t.desc.target = 1; t.desc.format = format; t.desc.width = t.desc.height = 64; t.desc.depth = t.desc.levels = 1; return t;
}
int main(int argc, char **argv) {
    @autoreleasepool {
        device = MTLCreateSystemDefaultDevice(); if (!device) return 77;
        queue = [device newCommandQueue]; targets = [NSMutableDictionary dictionary];
        wonder_host_draw_set_device((__bridge void *)device);
        const char *mode = argc > 1 ? argv[1] : "draw";
        // Vertex format table: NVN format -> MTLVertexFormat.
        assert(wonder_host_vertex_format(0x2E) == MTLVertexFormatFloat4 && wonder_host_vertex_format(0x16) == MTLVertexFormatFloat2);
        assert(wonder_host_vertex_format(0x29) == MTLVertexFormatHalf4 && wonder_host_vertex_format(0x25) == MTLVertexFormatUChar4Normalized);
        assert(wonder_host_vertex_format(0x27) == MTLVertexFormatUChar4 && wonder_host_vertex_format(0x22) == MTLVertexFormatFloat3);
        assert(wonder_host_vertex_format(0) == MTLVertexFormatInvalid);
        static struct wonder_shader_refl vr, fr; char err[128];
        const char *vt = "stage vs\nc1 2 00000000000000000000000000000000\nattr 0 0 float\n", *ft = "stage ps\ncbuf 0 1\ntex 0 1 1 2d\nfout 0 float\n";
        assert(!wonder_shader_refl_parse(vt, strlen(vt), &vr, err, sizeof err) && !wonder_shader_refl_parse(ft, strlen(ft), &fr, err, sizeof err));
        static struct wonder_host_draw d; memset(&d, 0, sizeof d);
        d.vs = (__bridge_retained void *)function(vs_src); d.fs = (__bridge_retained void *)function(fs_src); d.vr = &vr; d.fr = &fr;
        strcpy(d.vs_key, "1111111111111111"); strcpy(d.fs_key, "2222222222222222");
        d.targets[0].texture = target_texture(1, 0x25); d.targets[0].valid = 1;
        d.targets[1].texture = target_texture(2, 0x34); d.targets[1].valid = 1; d.color_count = 1;   // DEPTH32F
        // CCW triangle in NDC around the centre at z = 0.25 (front-facing for NVN front face CCW).
        float vertices[3][4] = {{-0.5f, -0.5f, 0.25f, 1}, {0.5f, -0.5f, 0.25f, 1}, {0, 0.5f, 0.25f, 1}};
        if (!strcmp(mode, "back_facing")) { float t[4]; memcpy(t, vertices[1], 16); memcpy(vertices[1], vertices[2], 16); memcpy(vertices[2], t, 16); }
        id<MTLBuffer> pool = [device newBufferWithLength:0x1000 options:MTLResourceStorageModeShared];
        memcpy(pool.contents, vertices, sizeof vertices);
        uint8_t indices[3] = {0, 1, 2}; memcpy((uint8_t *)pool.contents + 0x100, indices, 3);
        float tint[4] = {1, 0.5f, 1, 1}; memcpy((uint8_t *)pool.contents + 0x200, tint, 16);
        d.primitive = 4; d.index_type = 0; d.count = 3; d.instances = 1;
        d.index = (struct wonder_host_span){(__bridge void *)pool, 0x100, 3};
        d.nattr = 1; d.attr[0].format = !strcmp(mode, "bad_vertex_format") ? 0x31 : 0x2E; d.attr[0].offset = 0; d.attr[0].stream = 0;
        d.stream[0].span = (struct wonder_host_span){(__bridge void *)pool, 0, sizeof vertices}; d.stream[0].stride = 16; d.stream_mask = 1;
        d.ubo[1][0] = (struct wonder_host_span){(__bridge void *)pool, 0x200, 16};
        // 2x2 RGBA8 texture in TX1 block-linear storage: texel (0,1) (sampled at uv 0.25,0.75) is green.
        nvn_texdesc td = {.target = 1, .format = 0x25, .samples = 0, .width = 2, .height = 2, .depth = 1, .levels = 1};
        nvn_texlayout lay; nvn_tex_layout(&td, &lay);
        uint8_t linear[2][2][4] = {{{255, 0, 0, 255}, {255, 0, 0, 255}}, {{0, 255, 0, 255}, {255, 0, 0, 255}}};
        uint8_t *storage = calloc(1, lay.size); assert(!nvn_swizzle(&td, 0, 0, linear, 8, storage));
        d.tex_mask[1] = 1; d.tex[1][0].texture.valid = 1; d.tex[1][0].texture.texture.serial = 77; d.tex[1][0].texture.texture.desc = td;
        d.tex[1][0].texture.texture.swizzle[0] = 2; d.tex[1][0].texture.texture.swizzle[1] = 3; d.tex[1][0].texture.texture.swizzle[2] = 4; d.tex[1][0].texture.texture.swizzle[3] = 5;
        d.tex[1][0].storage = storage; d.tex[1][0].storage_size = lay.size;
        d.tex[1][0].sampler = (struct wonder_nvn_sampler_state){.min = 0, .mag = 0, .wrap_s = 7, .wrap_t = 7, .wrap_r = 7, .valid = 1, .lod_max = 1000, .anisotropy = 1};
        d.color = (wonder_nvn_color_state){.blend_en = 0, .logic_op = 3, .alpha_func = 8}; d.channel_mask.mask = 0xffffffff;
        d.depth_stencil = (wonder_nvn_depth_stencil_state){.dt = 1, .dw = 1, .df = 2};   // LESS
        d.polygon = (wonder_nvn_polygon_state){.cull = 2, .front = 1, .mode = 2};        // cull BACK, front CCW, FILL
        d.states_set = 1 | 2 | 4 | 8;
        float vp[4] = {0, 0, 64, 64}; memcpy(d.viewport, vp, 16); d.viewport_set = 1; d.depth_range[1] = 1;
        d.depth_mode = 0; d.window_origin = 1;
        // Clear colour to blue, depth to 1.0, then draw.
        id<MTLTexture> color = (__bridge id<MTLTexture>)wonder_host_render_target(&d.targets[0].texture, MTLPixelFormatRGBA8Unorm);
        id<MTLTexture> depth = (__bridge id<MTLTexture>)wonder_host_render_target(&d.targets[1].texture, MTLPixelFormatDepth32Float);
        MTLRenderPassDescriptor *clear = [MTLRenderPassDescriptor renderPassDescriptor];
        clear.colorAttachments[0].texture = color; clear.colorAttachments[0].loadAction = MTLLoadActionClear; clear.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
        clear.colorAttachments[0].storeAction = MTLStoreActionStore;
        clear.depthAttachment.texture = depth; clear.depthAttachment.loadAction = MTLLoadActionClear; clear.depthAttachment.clearDepth = 1; clear.depthAttachment.storeAction = MTLStoreActionStore;
        [[(__bridge id<MTLCommandBuffer>)wonder_host_replay_command_buffer() renderCommandEncoderWithDescriptor:clear] endEncoding];
        assert(wonder_host_draw(&d) == 0);
        if (strcmp(mode, "draw") && strcmp(mode, "back_facing")) return 4;   // stop cases must not get here
        char png[1024]; snprintf(png, sizeof png, "%s/target.png", argc > 2 ? argv[2] : "/tmp");
        if (!strcmp(mode, "draw")) assert(wonder_host_readback_png((__bridge void *)color, 0, 0, png) == 0);
        id<MTLBuffer> rb = [device newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared], zb = [device newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> b = [pending blitCommandEncoder];
        [b copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256 * 64];
        [b copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:zb destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256 * 64];
        [b endEncoding]; [pending commit]; [pending waitUntilCompleted]; assert(pending.status == MTLCommandBufferStatusCompleted);
        const uint8_t *px = rb.contents; const float *z = zb.contents;
        const uint8_t *centre = px + (32 * 64 + 32) * 4, *corner = px;
        printf("centre %u %u %u %u depth %g; corner %u %u %u %u depth %g\n", centre[0], centre[1], centre[2], centre[3], z[32 * 64 + 32], corner[0], corner[1], corner[2], corner[3], z[0]);
        assert(corner[0] == 0 && corner[1] == 0 && corner[2] == 255 && z[0] == 1.0f);   // untouched: clear colour and depth
        if (!strcmp(mode, "back_facing")) { assert(centre[2] == 255 && z[32 * 64 + 32] == 1.0f); puts("metal2 encode back face culled OK"); return 0; }
        assert(centre[0] == 0 && (centre[1] == 127 || centre[1] == 128) && centre[2] == 0 && centre[3] == 255);   // green texel x tint
        assert(z[32 * 64 + 32] == 0.25f);
        for (int i = 0; i < 200 && access(png, F_OK); i++) usleep(10000);   // written by the command buffer's completion handler
        NSData *file = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:png]];
        assert(file.length > 8 && !memcmp(file.bytes, "\x89PNG", 4));
        CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)file, NULL); CGImageRef image = CGImageSourceCreateImageAtIndex(src, 0, NULL);
        assert(CGImageGetWidth(image) == 64 && CGImageGetHeight(image) == 64);
        CFDataRef pixels = CGDataProviderCopyData(CGImageGetDataProvider(image)); const uint8_t *p = CFDataGetBytePtr(pixels) + 32 * CGImageGetBytesPerRow(image) + 32 * 4;
        assert(p[0] == centre[0] && p[1] == centre[1] && p[2] == centre[2]);
        CFRelease(pixels); CGImageRelease(image); CFRelease(src);
        puts("metal2 encode draw OK");
    }
    return 0;
}
