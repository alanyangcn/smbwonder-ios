#pragma once
// One draw, fully resolved by the replay (nvn_queue.c) for the Metal backend (metal3_draw.m): shader functions and their
// reflections, host buffer spans for every buffer the reflections and vertex layout read, texture/sampler descriptors
// snapshotted from the bound pools, render targets and fixed-function state. Everything is by value for one submission.
#include "nvn_commands.h"
#include "nvn_state.h"
#include "shader_library.h"
struct wonder_host_span { void *buffer; size_t offset, size; };   // host MTLBuffer of the pool + byte offset
struct wonder_host_draw {
    void *vs, *fs; const struct wonder_shader_refl *vr, *fr; char vs_key[17], fs_key[17];
    struct wonder_nvn_texture_snapshot targets[9]; int color_count;
    int primitive, index_type, count, first, base_vertex, instances, base_instance;   // index_type -1 = DrawArrays
    struct wonder_host_span index;
    int nattr; struct { uint32_t format, offset, stream; } attr[16];                 // indexed like vr->attr; stream DEFAULT = disabled
#define WONDER_HOST_DEFAULT_ATTRIBUTE 0xffu   // disabled NVN attribute: reads (0,0,0,1) from the zero buffer at [[buffer(15)]]
    // fetch_size: bytes the draw reads from the stream start; beyond span.size Maxwell returns zeros (the host pads).
    struct { struct wonder_host_span span; uint32_t stride, divisor; uint64_t fetch_size; } stream[16]; uint16_t stream_mask;
    struct wonder_host_span ubo[2][14];                                               // [stage][NVN uniform slot], read slots only
    struct wonder_host_span sbuf[2][16];                                              // [stage][NVN storage slot], used slots only
    struct { struct wonder_nvn_texture_registration texture; struct wonder_nvn_sampler_state sampler;
             const uint8_t *storage; uint64_t storage_size;
             void *storage_buffer; uint64_t storage_buffer_offset;
             int empty; } tex[2][40]; uint64_t tex_mask[2];   // storage_buffer: the pool's MTLBuffer; empty: handle 0 (reads 0)   // slots 32.. = bindless; storage: pool bytes (TX1 layout)
    wonder_nvn_blend_state blend[8]; uint8_t blend_set; float blend_color[4];   // SetBlendColor (constant blend factors)
    wonder_nvn_color_state color; wonder_nvn_channel_mask_state channel_mask; wonder_nvn_depth_stencil_state depth_stencil;
    wonder_nvn_polygon_state polygon; wonder_nvn_multisample_state multisample; uint8_t states_set;   // bits as wonder_nvn_draw_state
    float viewport[4]; int viewport_set; float depth_range[2]; int scissor[4]; int scissor_set;
    int depth_mode, window_origin;
    int depth_bias_enabled; float depth_bias, depth_bias_slope, depth_bias_clamp;   // NVN polygon offset (units, factor, clamp)
    int depth_clamp;
    unsigned frame;    // presents so far (diagnostics)   // nvnCommandBufferSetDepthClamp: no near / far clipping, depth clamped to the range (MTLDepthClipModeClamp)
    // Driver constant segment c[0] (bytes 0..0x700) for stages whose reflection has "drv 0 <buffer>": buffer address
    // tables (storage vertex 0x110 / compute 0x310 / fragment 0x510, compute uniforms 0x210; 16 bytes per slot:
    // u64 NVN GPU address, u32 size), filled by the replay from the bound buffers.
    uint8_t drv0[0x700];
};
// One compute dispatch, resolved by the replay like a draw. Textures and samplers are in res.tex[0][slot] (res is the
// texture context sampled_texture() reads; res.vs_key holds the compute key for messages). Images are GPU-resident
// textures (host: id<MTLTexture>, created with shader-write usage for NVN_TEXTURE_FLAGS_IMAGE textures).
struct wonder_host_dispatch {
    void *cs; const struct wonder_shader_refl *r; char key[17];
    int groups[3]; int indirect; struct wonder_host_span indirect_span;    // indirect: the GPU reads the 3 group counts
    struct wonder_host_span ubo[14], uptr[14], sbuf[16];                  // [NVN uniform / storage slot], read slots only
    struct wonder_host_draw res;
    struct { struct wonder_nvn_texture_snapshot image; void *host; } img[8]; uint8_t img_mask;
};
int wonder_host_dispatch(const struct wonder_host_dispatch *x);            // 0 = encoded; unsupported state stops by name
int wonder_host_shader_available(const char key[17]);                       // 1 = in the bundled shader build
void *wonder_host_shader_function(const char key[17], int stage);           // id<MTLFunction> (stage 0 vertex, 1 fragment, 5 kernel); stops by name on failure
const struct wonder_shader_refl *wonder_host_shader_refl(const char key[17]);
int wonder_host_draw(const struct wonder_host_draw *d);                     // 0 = encoded; unsupported state stops by name
uint32_t wonder_host_vertex_format(int nvn_format);
// Colour clear limited to rect {x, y, w, h} and channel mask (NVN R1 G2 B4 A8), drawn as a triangle (Metal load-clears
// cover the whole attachment). Float/normalized formats only; 0 = encoded.
int wonder_host_clear_rect(void *texture, int level, int slice, const float rgba[4], unsigned mask, const int rect[4]);
// Depth / stencil clear limited to rect, drawn as a triangle at the clear depth (compare always; stencil replaced under
// stencil_mask). 0 = encoded.
int wonder_host_clear_depth_stencil_rect(void *texture, int level, int slice, int clear_depth, float depth, int clear_stencil,
                                         uint32_t stencil, uint32_t stencil_mask, const int rect[4]);                          // MTLVertexFormat, 0 = none
// Replay command buffer shared with host_graphics.m (clears, fills, present): the draw encoder appends to it under the lock.
void wonder_host_replay_lock(void);
void wonder_host_replay_unlock(void);
void *wonder_host_replay_command_buffer(void);                               // pending id<MTLCommandBuffer>, call under the lock
// Deferred work (metal3_draw.m operation queue, planned for load/store actions when the command buffer is next taken),
// under the lock: a full clear (render pass descriptor with Clear / Load attachments, no draws) and buffer-only work.
void wonder_host_draw_queue_clear_locked(void *render_pass_descriptor);
#ifdef __OBJC__
@protocol MTLCommandBuffer;
void wonder_host_draw_queue_work_locked(void (^work)(id<MTLCommandBuffer>));
#endif
void wonder_host_tbdr_window(char *out, size_t size);                        // HOST_MEMORY planning counters (WONDER_TBDR)
void *wonder_host_render_target_existing(const struct wonder_nvn_texture *t); // GPU-written texture for this serial, or NULL
// Evidence: after the submit holding the first executed draw, its first colour target is written as a PNG once the GPU
// is done (host_graphics.m: Documents/FirstDraw/target-0.png). RGBA8/BGRA8 (+sRGB) only; other formats return non-zero.
int wonder_host_readback_png(void *texture, int level, int slice, const char *path);
int wonder_host_readback_png_rect(void *texture, int level, int slice, const int rect[4], const char *path);   // rect NULL = whole level
void wonder_host_capture_first_draw(const struct wonder_nvn_texture_snapshot *target);
int wonder_host_gpu_error(char *out, size_t size);
void wonder_host_note_submit(unsigned submit);
void wonder_host_gpu_error_reset(void);
// Runs fn(arg) on the host replay thread (16 MiB stack) and waits: guest threads have small stacks, and the replay
// (nested CallCommands, Metal encoders) needs more (round85: ___chkstk_darwin in renderCommandEncoderWithDescriptor).
void wonder_host_run_on_replay_stack(void (*fn)(void *), void *arg);              // diagnostic bisect only (WONDER_DRAW_SYNC)       // labels the pending command buffer with the submits it holds (GPU error reports)   // 1 = a committed replay command buffer failed (text in out)
