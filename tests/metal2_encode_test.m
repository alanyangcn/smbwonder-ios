// wonder_host_draw on a real Metal device: one resolved draw with a block-linear texture, a uniform buffer, c1 constants,
// u8 indices, back-face culling (NVN CCW front) and a LESS depth test; colour and depth are read back.
// argv[1] = "draw" | "bad_vertex_format" | "back_facing" (each run in its own process: Metal is not fork-safe).
#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
#include <unistd.h>
#include <pthread.h>
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
// Like host_graphics.m: one non-recursive replay mutex, also taken by the render-target lookups; re-entry aborts.
static pthread_mutex_t replay = PTHREAD_MUTEX_INITIALIZER;
static void take(const char *who) { if (pthread_mutex_trylock(&replay)) { fprintf(stderr, "replay mutex re-entered by %s (deadlock on device)\n", who); abort(); } }
void wonder_host_gpu_error_reset(void) {} void wonder_host_finish(void) {} int wonder_host_gpu_error(char *out, size_t size) { (void)out; (void)size; return 0; }
void wonder_host_replay_lock(void) { take("wonder_host_replay_lock"); } void wonder_host_replay_unlock(void) { pthread_mutex_unlock(&replay); }
void *wonder_host_replay_command_buffer(void) { if (!pending) pending = [queue commandBuffer]; return (__bridge void *)pending; }
void *wonder_host_render_target_existing(const struct wonder_nvn_texture *t) { take("render_target_existing"); id<MTLTexture> x = targets[@(t->serial)]; pthread_mutex_unlock(&replay); return (__bridge void *)x; }
void *wonder_host_render_target(const struct wonder_nvn_texture *t, uint32_t format) {
    take("render_target"); pthread_mutex_unlock(&replay);
    id<MTLTexture> x = targets[@(t->serial)];
    if (!x) { MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)format width:t->desc.width height:t->desc.height mipmapped:NO];
              d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate; x = [device newTextureWithDescriptor:d]; targets[@(t->serial)] = x; }
    return (__bridge void *)x;
}
static NSString *const vs_src = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "struct I { float4 p [[attribute(0)]]; };\nstruct O { float4 p [[position]]; };\nstruct C1 { float4 v[4]; };\n"
    "vertex O main0(I in [[stage_in]], constant C1& c1 [[buffer(2)]]) { O o; o.p = in.p + c1.v[0]; return o; }\n";
// The uniform block declares more than the game binds (like translated Maxwell c[n] arrays): reads past the bound size
// must see zeros, as on Maxwell, not neighbouring pool bytes.
static NSString *const fs_src = @"#include <metal_stdlib>\nusing namespace metal;\nstruct U { float4 tint; float4 beyond; };\n"
    "fragment float4 main0(constant U& u [[buffer(1)]], texture2d<float> t [[texture(1)]], sampler s [[sampler(1)]]) { return t.sample(s, float2(0.25, 0.75)) * u.tint + u.beyond; }\n";
static NSString *const fs_cube_src = @"#include <metal_stdlib>\nusing namespace metal;\nstruct U { float4 tint; float4 beyond; };\n"
    "fragment float4 main0(constant U& u [[buffer(1)]], texturecube<float> t [[texture(1)]], sampler s [[sampler(1)]]) { return t.sample(s, float3(1, 0, 0)) * u.tint; }\n";
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
        // Render target descriptors by NVN target: 2D, 2D array, rectangle, cube and cube array (depth = layer-faces).
        {
            nvn_texdesc c = {.target = 8, .format = 0x3f, .width = 128, .height = 128, .depth = 6, .levels = 8};
            MTLTextureDescriptor *x = wonder_host_render_target_descriptor(&c, MTLPixelFormatRG11B10Float);
            assert(x && x.textureType == MTLTextureTypeCube && x.mipmapLevelCount == 8 && x.arrayLength == 1);
            c.target = 9; c.depth = 12; x = wonder_host_render_target_descriptor(&c, MTLPixelFormatRG11B10Float);
            assert(x && x.textureType == MTLTextureTypeCubeArray && x.arrayLength == 2);
            c.target = 4; c.depth = 3; x = wonder_host_render_target_descriptor(&c, MTLPixelFormatRGBA8Unorm);
            assert(x && x.textureType == MTLTextureType2DArray && x.arrayLength == 3);
            c.target = 2; c.depth = 16; x = wonder_host_render_target_descriptor(&c, MTLPixelFormatRGBA8Unorm);   // 3D: z slices via depthPlane
            assert(x && x.textureType == MTLTextureType3D && x.depth == 16 && x.arrayLength == 1);
            c.target = 5; assert(!wonder_host_render_target_descriptor(&c, MTLPixelFormatRGBA8Unorm));   // multisample: not yet
            c.target = 9; c.depth = 7; assert(!wonder_host_render_target_descriptor(&c, MTLPixelFormatRGBA8Unorm));   // not whole cubes
            id<MTLTexture> cube = [device newTextureWithDescriptor:wonder_host_render_target_descriptor(&(nvn_texdesc){.target = 8, .format = 0x25, .width = 4, .height = 4, .depth = 6, .levels = 1}, MTLPixelFormatRGBA8Unorm)];
            assert(cube && cube.textureType == MTLTextureTypeCube);
        }
        // Scissored depth clear drawn as a triangle: D32F cleared to 1.0, rect (16,16,32,32) cleared to 0.25.
        if (!strcmp(mode, "clear_depth_rect")) {
            struct wonder_nvn_texture zt = target_texture(10, 0x34); id<MTLTexture> z = (__bridge id<MTLTexture>)wonder_host_render_target(&zt, MTLPixelFormatDepth32Float);
            MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor]; p.depthAttachment.texture = z;
            p.depthAttachment.loadAction = MTLLoadActionClear; p.depthAttachment.clearDepth = 1.0; p.depthAttachment.storeAction = MTLStoreActionStore;
            [[(__bridge id<MTLCommandBuffer>)wonder_host_replay_command_buffer() renderCommandEncoderWithDescriptor:p] endEncoding];
            int rect[4] = {16, 16, 32, 32};
            assert(wonder_host_clear_depth_stencil_rect((__bridge void *)z, 0, 0, 1, 0.25f, 0, 0, 0xff, rect) == 0);
            id<MTLBuffer> zb = [device newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> b = [pending blitCommandEncoder];
            [b copyFromTexture:z sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:zb destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256 * 64];
            [b endEncoding]; [pending commit]; [pending waitUntilCompleted];
            const float *zz = zb.contents; printf("depth in %g outside %g edge %g\n", zz[32 * 64 + 32], zz[8 * 64 + 8], zz[48 * 64 + 48]);
            assert(zz[32 * 64 + 32] == 0.25f && zz[8 * 64 + 8] == 1.0f && zz[48 * 64 + 48] == 1.0f);
            puts("metal2 clear depth rect OK"); return 0;
        }
        // Scissored / channel-masked colour clear drawn as a triangle: rect (16,16,32,32), red+alpha only, over blue.
        if (!strcmp(mode, "clear_rect")) {
            struct wonder_nvn_texture ct = target_texture(9, 0x25); id<MTLTexture> c = (__bridge id<MTLTexture>)wonder_host_render_target(&ct, MTLPixelFormatRGBA8Unorm);
            MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor]; p.colorAttachments[0].texture = c;
            p.colorAttachments[0].loadAction = MTLLoadActionClear; p.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1); p.colorAttachments[0].storeAction = MTLStoreActionStore;
            [[(__bridge id<MTLCommandBuffer>)wonder_host_replay_command_buffer() renderCommandEncoderWithDescriptor:p] endEncoding];
            float red[4] = {1, 0.25f, 0, 0.5f}; int rect[4] = {16, 16, 32, 32};
            assert(wonder_host_clear_rect((__bridge void *)c, 0, 0, red, 0x9, rect) == 0);
            id<MTLBuffer> rb = [device newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> b = [pending blitCommandEncoder];
            [b copyFromTexture:c sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256 * 64];
            [b endEncoding]; [pending commit]; [pending waitUntilCompleted];
            const uint8_t *in = (const uint8_t *)rb.contents + (32 * 64 + 32) * 4, *outside = (const uint8_t *)rb.contents + (8 * 64 + 8) * 4, *edge = (const uint8_t *)rb.contents + (48 * 64 + 48) * 4;
            printf("in %u %u %u %u outside %u %u %u %u\n", in[0], in[1], in[2], in[3], outside[0], outside[1], outside[2], outside[3]);
            assert(in[0] == 255 && in[1] == 0 && in[2] == 255 && (in[3] == 127 || in[3] == 128));   // R and A written, G/B kept
            assert(outside[0] == 0 && outside[2] == 255 && edge[0] == 0);                         // outside the rect untouched (x,y 48 excluded)
            puts("metal2 clear rect OK"); return 0;
        }
        // Texture view slice ranges in Metal slices (round83: Metal requires whole cubes for cube / cube-array views;
        // NVN layers are layer-faces). Out-of-range layers return length 0.
        {
            NSRange r;
            r = wonder_host_view_slices(MTLTextureType2D, 1, 0, 0);            assert(r.location == 0 && r.length == 1);
            r = wonder_host_view_slices(MTLTextureType2DArray, 4, 1, 2);       assert(r.location == 1 && r.length == 2);
            r = wonder_host_view_slices(MTLTextureType2DArray, 4, 0, 0);       assert(r.location == 0 && r.length == 4);   // layers 0 = all
            r = wonder_host_view_slices(MTLTextureTypeCube, 1, 0, 1);          assert(r.location == 0 && r.length == 6);   // a cube is 6 slices
            r = wonder_host_view_slices(MTLTextureTypeCubeArray, 2, 0, 0);     assert(r.location == 0 && r.length == 12);
            r = wonder_host_view_slices(MTLTextureTypeCubeArray, 2, 6, 6);     assert(r.location == 6 && r.length == 6);
            r = wonder_host_view_slices(MTLTextureTypeCubeArray, 2, 0, 1);     assert(r.location == 0 && r.length == 6);   // rounded up to the cube
            r = wonder_host_view_slices(MTLTextureTypeCubeArray, 2, 3, 1);     assert(r.length == 0);                       // not cube-aligned
            r = wonder_host_view_slices(MTLTextureType2DArray, 4, 3, 2);       assert(r.length == 0);                       // beyond the array
        }
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
        float tint[4] = {1, 0.5f, 1, 1}, garbage[4] = {1, 1, 1, 1}; memcpy((uint8_t *)pool.contents + 0x200, tint, 16); memcpy((uint8_t *)pool.contents + 0x210, garbage, 16);
        d.primitive = 4; d.index_type = 0; d.count = 3; d.instances = 1;
        d.index = (struct wonder_host_span){(__bridge void *)pool, 0x100, 3};
        d.nattr = 1; d.attr[0].format = !strcmp(mode, "bad_vertex_format") ? 0x31 : 0x2E; d.attr[0].offset = 0; d.attr[0].stream = 0;
        d.stream[0].span = (struct wonder_host_span){(__bridge void *)pool, 0, sizeof vertices}; d.stream[0].stride = 16; d.stream_mask = 1;
        d.stream[0].fetch_size = sizeof vertices;
        if (!strcmp(mode, "fetch_pad")) {
            // Index 3 is past the 3 bound vertices: Maxwell reads zeros. The pool holds a far-away vertex there; the zero
            // vertex (0,0,0,0) makes a degenerate triangle with the others -> centre still drawn by the first triangle only.
            float far[4] = {5, 5, 0.25f, 1}; memcpy((uint8_t *)pool.contents + sizeof vertices, far, 16);
            uint8_t six[6] = {0, 1, 2, 0, 1, 3}; memcpy((uint8_t *)pool.contents + 0x100, six, 6); d.count = 6; d.index.size = 6;
            d.stream[0].fetch_size = 4 * 16;
        }
        d.ubo[1][0] = (struct wonder_host_span){(__bridge void *)pool, 0x200, 16};
        // 2x2 RGBA8 texture in TX1 block-linear storage: texel (0,1) (sampled at uv 0.25,0.75) is green.
        nvn_texdesc td = {.target = 1, .format = 0x25, .samples = 0, .width = 2, .height = 2, .depth = 1, .levels = 1};
        nvn_texlayout lay; nvn_tex_layout(&td, &lay);
        uint8_t linear[2][2][4] = {{{255, 0, 0, 255}, {255, 0, 0, 255}}, {{0, 255, 0, 255}, {255, 0, 0, 255}}};
        uint8_t *storage = calloc(1, lay.size); assert(!nvn_swizzle(&td, 0, 0, linear, 8, storage));
        d.tex_mask[1] = 1; d.tex[1][0].texture.valid = 1; d.tex[1][0].texture.texture.serial = 77; d.tex[1][0].texture.texture.desc = td;
        d.tex[1][0].texture.texture.swizzle[0] = 2; d.tex[1][0].texture.texture.swizzle[1] = 3; d.tex[1][0].texture.texture.swizzle[2] = 4; d.tex[1][0].texture.texture.swizzle[3] = 5;
        d.tex[1][0].storage = storage; d.tex[1][0].storage_size = lay.size;
        int swizzle_views = [device supportsFamily:MTLGPUFamilyApple1] || [device supportsFamily:MTLGPUFamilyMac2];   // A11 yes; old Intel Macs no
        if (!strcmp(mode, "blend_color")) {
            // Target 0 blends with CONSTANT_COLOR / ZERO: result = source x blend colour (0.5, 1, 1, 1) -> red channel halves.
            d.color.blend_en = 1; d.blend_set = 1;
            d.blend[0] = (wonder_nvn_blend_state){.target = 0, .src_rgb = 0x61, .dst_rgb = 1, .src_a = 2, .dst_a = 1, .eq_rgb = 1, .eq_a = 1};
            d.blend_color[0] = 0.5f; d.blend_color[1] = d.blend_color[2] = d.blend_color[3] = 1.0f;
            float tint2[4] = {1, 1, 1, 1}; memcpy((uint8_t *)pool.contents + 0x200, tint2, 16);
            uint8_t white[2][2][4] = {{{255, 255, 255, 255}, {255, 255, 255, 255}}, {{255, 255, 255, 255}, {255, 255, 255, 255}}};
            assert(!nvn_swizzle(&td, 0, 0, white, 8, storage)); d.tex[1][0].texture.texture.serial = 80;
        }
        if (!strcmp(mode, "cube_view")) {
            // Sampling a GPU-written cube render target through a level view (round83: a 1-slice view of a cube array aborts).
            struct wonder_nvn_texture ct = {0}; ct.serial = 90; ct.desc = (nvn_texdesc){.target = 8, .format = 0x25, .width = 4, .height = 4, .depth = 6, .levels = 2};
            id<MTLTexture> cube = (__bridge id<MTLTexture>)wonder_host_render_target(&ct, MTLPixelFormatRGBA8Unorm);
            for (int face = 0; face < 6; face++) {
                MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor]; p.colorAttachments[0].texture = cube; p.colorAttachments[0].slice = face;
                p.colorAttachments[0].loadAction = MTLLoadActionClear; p.colorAttachments[0].clearColor = MTLClearColorMake(0, 1, 0, 1); p.colorAttachments[0].storeAction = MTLStoreActionStore;
                [[(__bridge id<MTLCommandBuffer>)wonder_host_replay_command_buffer() renderCommandEncoderWithDescriptor:p] endEncoding];
            }
            d.fs = (__bridge_retained void *)function(fs_cube_src);
            fr.tex[0].type = WONDER_SHADER_TEX_CUBE;
            d.tex[1][0].texture.texture = ct; d.tex[1][0].storage = NULL;
            struct wonder_nvn_texture_view *v = &d.tex[1][0].texture.view; d.tex[1][0].texture.has_view = 1;
            v->base_level = 0; v->levels = 1; v->set_levels = 1;
        }
        if (!strcmp(mode, "bc")) {
            // 4x4 BC1 texture, one solid green block (565 0x07e0), decoded on the CPU as on the A11 (no BC sampling there).
            setenv("WONDER_TEST_NO_BC", "1", 1);
            nvn_texdesc bd = {.target = 1, .format = 0x42, .samples = 0, .width = 4, .height = 4, .depth = 1, .levels = 1};
            nvn_texlayout bl; nvn_tex_layout(&bd, &bl);
            uint8_t block[8] = {0xe0, 0x07, 0xe0, 0x07, 0, 0, 0, 0}; uint8_t *bs = calloc(1, bl.size); assert(!nvn_swizzle(&bd, 0, 0, block, 8, bs));
            d.tex[1][0].texture.texture.desc = bd; d.tex[1][0].storage = bs; d.tex[1][0].storage_size = bl.size; d.tex[1][0].texture.texture.serial = 79;
        }
        if (!strcmp(mode, "view") || !strcmp(mode, "bad_view")) {
            // Every view field set, as round56's second draw registers it. Level 1 (1x1 red) is selected; where the GPU has
            // texture swizzle, blue is taken from red, so the red texel samples magenta.
            td.levels = 2; nvn_tex_layout(&td, &lay);
            uint8_t *mips = calloc(1, lay.size); assert(!nvn_swizzle(&td, 0, 0, linear, 8, mips));
            uint8_t red[4] = {255, 0, 0, 255}; assert(!nvn_swizzle(&td, 1, 0, red, 4, mips));
            d.tex[1][0].texture.texture.desc = td; d.tex[1][0].storage = mips; d.tex[1][0].storage_size = lay.size; d.tex[1][0].texture.texture.serial = 78;
            struct wonder_nvn_texture_view *v = &d.tex[1][0].texture.view; d.tex[1][0].texture.has_view = 1;
            v->target = 1; v->format = !strcmp(mode, "bad_view") ? 0x34 : 0x25; v->base_level = 1; v->levels = 1; v->min_layer = 0; v->layers = 1;
            v->set_levels = v->set_layers = v->set_format = v->set_swizzle = v->set_depth_stencil = v->set_target = 1;
            v->swizzle[0] = 2; v->swizzle[1] = 3; v->swizzle[2] = swizzle_views ? 2 : 4; v->swizzle[3] = 5;
        }
        d.tex[1][0].sampler = (struct wonder_nvn_sampler_state){.min = 0, .mag = 0, .wrap_s = 7, .wrap_t = 7, .wrap_r = 7, .valid = 1, .lod_max = 1000, .anisotropy = 1};
        d.color = (wonder_nvn_color_state){.blend_en = 0, .logic_op = 3, .alpha_func = 8};
        if (!strcmp(mode, "blend_color")) d.color.blend_en = 1; d.channel_mask.mask = 0;   // never bound: zeroed like the replay's draw state
        d.depth_stencil = (wonder_nvn_depth_stencil_state){.dt = 1, .dw = 1, .df = 2};   // LESS
        d.polygon = (wonder_nvn_polygon_state){.cull = 2, .front = 1, .mode = 2};        // cull BACK, front CCW, FILL
        d.states_set = 1 | 4 | 8;   // channel mask never bound: all channels written
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
        if (!strcmp(mode, "mask")) {
            // Same shaders and targets, channel mask now bound to 0: must not reuse the write-all pipeline of the unset mask.
            [[(__bridge id<MTLCommandBuffer>)wonder_host_replay_command_buffer() renderCommandEncoderWithDescriptor:clear] endEncoding];
            d.states_set |= 2; d.channel_mask.mask = 0;
            assert(wonder_host_draw(&d) == 0);
        }
        if (strcmp(mode, "draw") && strcmp(mode, "back_facing") && strcmp(mode, "mask") && strcmp(mode, "view") && strcmp(mode, "bc") && strcmp(mode, "blend_color") && strcmp(mode, "fetch_pad") && strcmp(mode, "cube_view")) return 4;   // stop cases must not get here
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
        if (!strcmp(mode, "mask")) { assert(centre[0] == 0 && centre[1] == 0 && centre[2] == 255); puts("metal2 encode channel mask OK"); return 0; }
        if (!strcmp(mode, "cube_view")) { assert(centre[0] == 0 && (centre[1] == 127 || centre[1] == 128) && centre[2] == 0); puts("metal2 encode cube view OK"); return 0; }
        if (!strcmp(mode, "fetch_pad")) {   // the corner would be covered if vertex 3 came from the pool bytes (5,5)
            const uint8_t *far_px = px + (2 * 64 + 62) * 4; assert(far_px[2] == 255 && far_px[1] == 0); puts("metal2 encode fetch padding OK"); return 0; }
        if (!strcmp(mode, "blend_color")) { assert((centre[0] == 127 || centre[0] == 128) && centre[1] == 255 && centre[2] == 255); puts("metal2 encode blend colour OK"); return 0; }
        if (!strcmp(mode, "bc")) { assert(centre[0] == 0 && (centre[1] == 127 || centre[1] == 128) && centre[2] == 0); puts("metal2 encode BC decode OK"); return 0; }
        if (!strcmp(mode, "view")) { assert(centre[0] == 255 && centre[1] == 0 && centre[2] == (swizzle_views ? 255 : 0)); printf("swizzle views %d\n", swizzle_views); puts("metal2 encode texture view OK"); return 0; }
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
