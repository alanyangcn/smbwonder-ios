#pragma once
// One draw, fully resolved by the replay (nvn_queue.c) for the Metal backend (metal2_draw.m): shader functions and their
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
    int nattr; struct { uint32_t format, offset, stream; } attr[16];                 // indexed like vr->attr
    struct { struct wonder_host_span span; uint32_t stride, divisor; } stream[16]; uint16_t stream_mask;
    struct wonder_host_span ubo[2][14];                                               // [stage][NVN uniform slot], read slots only
    struct { struct wonder_nvn_texture_registration texture; struct wonder_nvn_sampler_state sampler;
             const uint8_t *storage; uint64_t storage_size; } tex[2][32]; uint32_t tex_mask[2];   // storage: the texture's pool bytes (TX1 layout)
    wonder_nvn_blend_state blend[8]; uint8_t blend_set;
    wonder_nvn_color_state color; wonder_nvn_channel_mask_state channel_mask; wonder_nvn_depth_stencil_state depth_stencil;
    wonder_nvn_polygon_state polygon; wonder_nvn_multisample_state multisample; uint8_t states_set;   // bits as wonder_nvn_draw_state
    float viewport[4]; int viewport_set; float depth_range[2]; int scissor[4]; int scissor_set;
    int depth_mode, window_origin;
};
int wonder_host_shader_available(const char key[17]);                       // 1 = in the bundled shader build
void *wonder_host_shader_function(const char key[17], int stage);           // id<MTLFunction>; stops by name on failure
const struct wonder_shader_refl *wonder_host_shader_refl(const char key[17]);
int wonder_host_draw(const struct wonder_host_draw *d);                     // 0 = encoded; unsupported state stops by name
uint32_t wonder_host_vertex_format(int nvn_format);                          // MTLVertexFormat, 0 = none
// Replay command buffer shared with host_graphics.m (clears, fills, present): the draw encoder appends to it under the lock.
void wonder_host_replay_lock(void);
void wonder_host_replay_unlock(void);
void *wonder_host_replay_command_buffer(void);                               // pending id<MTLCommandBuffer>, call under the lock
void *wonder_host_render_target_existing(const struct wonder_nvn_texture *t); // GPU-written texture for this serial, or NULL
// Evidence: after the submit holding the first executed draw, its first colour target is written as a PNG once the GPU
// is done (host_graphics.m: Documents/FirstDraw/target-0.png). RGBA8/BGRA8 (+sRGB) only; other formats return non-zero.
int wonder_host_readback_png(void *texture, int level, int slice, const char *path);
void wonder_host_capture_first_draw(const struct wonder_nvn_texture_snapshot *target);
int wonder_host_gpu_error(char *out, size_t size);   // 1 = a committed replay command buffer failed (text in out)
