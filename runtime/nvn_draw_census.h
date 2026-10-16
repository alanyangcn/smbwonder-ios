#pragma once
// Draw census: before the replay stops at a draw it cannot execute yet, everything the offline shader build needs is
// written once per program (Documents/DrawCensus/<vs key>-<fs key>/ on device): draw.json (draw call, program keys, vertex layout,
// buffers, textures/samplers resolved through the bound pools, render targets, fixed-function state, depth/origin
// modes) plus stage-<n>.control.bin (0x880 bytes) and stage-<n>.code.bin (control +0x704 bytes from the code
// address: NVN header 0x30 + SPH 0x50 + instructions + c1; +0x700 is where c1 starts).
// Key = first 16 hex digits of sha256(control[0:0x880]) (same rule as D2R dm_ctl_sha16).
#include "nvn_texture.h"
#include "nvn_state.h"
#include "nvn_commands.h"
#define WONDER_NVN_SHADER_CONTROL_SIZE 0x880
struct wonder_nvn_draw_state {
    struct wonder_nvn_program_shaders program; int has_program;
    int nattrib, nstream;
    wonder_nvn_vertex_attrib_state attrib[16]; wonder_nvn_vertex_stream_state stream[16];
    struct { uint64_t address, size; } vertex_buffer[16], uniform_buffer[6][16], storage_buffer[6][16];
    uint64_t texture_handle[6][32];   // 0 = unbound
    uint64_t image_handle[6][8];      // BindImage: image id in the texture pool (nvnDeviceGetImageHandle); 0 = unbound
    const void *texture_pool, *sampler_pool;
    wonder_nvn_blend_state blend[8]; uint8_t blend_set;   // per blend target
    wonder_nvn_color_state color; wonder_nvn_channel_mask_state channel_mask; wonder_nvn_depth_stencil_state depth_stencil;
    wonder_nvn_polygon_state polygon; wonder_nvn_multisample_state multisample; uint8_t states_set;   // bits: color, mask, ds, polygon, ms
    struct wonder_nvn_texture_snapshot targets[9]; int color_count, has_targets;
    float viewport[4]; int viewport_set; int scissor[4]; int scissor_set;
};
struct wonder_nvn_draw_call { const char *name; int primitive, index_type, count, first, base_vertex, instances, base_instance; uint64_t index_address, indirect_address;
                              int compute, groups[3]; };   // compute: a dispatch (groups x/y/z, or indirect_address); the census covers the compute stage only
// Compute stage control (survey of 58 compute controls in Lib/agl, Lib/gsys, Effect esetb): threadgroup size x/y/z at +0x748,
// shared memory bytes at +0x710 (0x400 where the shader uses shared memory, 0 otherwise).
#define WONDER_NVN_CONTROL_LOCAL_SIZE 0x748
#define WONDER_NVN_CONTROL_SHARED_SIZE 0x710
void wonder_nvn_shader_key(const void *control, char key[17]);
// 1 = census written now, 0 = nothing written (no program bound, or this program already written this run), -1 = a write failed.
// summary receives "census=DrawCensus vs=<key> fs=<key>" (or the reason nothing was written).
int wonder_nvn_draw_census(const struct wonder_nvn_draw_state *s, const struct wonder_nvn_draw_call *d, char *summary, size_t summary_size);
int wonder_host_census_write(const char *name, const void *data, size_t size);   // supplied by the host; 0 = ok
