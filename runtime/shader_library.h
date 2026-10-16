#pragma once
// Translated-shader reflection (.refl, written by tools/wonder_shader_build.py next to <key>.metal).
// Metal binding layout per stage (MSL 3.2, discrete slots): constant buffers / c1 / support in [[buffer(0..15)]],
// vertex streams in [[buffer(16 + stream)]], textures/samplers at the translator's binding numbers.
#include <stddef.h>
#include <stdint.h>
enum { WONDER_SHADER_FLOAT, WONDER_SHADER_UINT, WONDER_SHADER_SINT };
enum { WONDER_SHADER_TEX_1D, WONDER_SHADER_TEX_2D, WONDER_SHADER_TEX_3D, WONDER_SHADER_TEX_1D_ARRAY, WONDER_SHADER_TEX_2D_ARRAY,
       WONDER_SHADER_TEX_2D_MS, WONDER_SHADER_TEX_2D_MS_ARRAY, WONDER_SHADER_TEX_CUBE, WONDER_SHADER_TEX_CUBE_ARRAY, WONDER_SHADER_TEX_BUFFER };
#define WONDER_SHADER_STREAM_BUFFER_BASE 16
#define WONDER_SHADER_DEFAULT_ATTRIBUTE_BUFFER 15   // zero buffer for disabled vertex attributes; shader buffers use 0..14
struct wonder_shader_refl {
    int stage;                                               // 0 vertex, 1 fragment, 5 compute (NVNshaderStage)
    int ncbuf; struct { uint8_t slot, buffer; } cbuf[14];    // NVN uniform slot r -> [[buffer(n)]]
    int support;                                             // Ryujinx support buffer [[buffer(n)]], -1 = none
    int c1_buffer; uint16_t c1_len; uint8_t c1[4096];         // shader's own constants (c[1]) -> [[buffer(n)]], -1 = none
    // NVN texture slot r -> [[texture]] / [[sampler]]; bindless entries (handle read from uniform slot ubo_slot at byte
    // offset) use slots 32 + n.
    int ntex; struct wonder_shader_tex { uint8_t slot, texture, sampler, type, bindless, ubo_slot; uint16_t offset; } tex[40];
    int nattr; struct { uint8_t location, nvn, type; } attr[16];         // [[attribute(location)]] <- NVN vertex attribute
    int nsbuf; struct { uint8_t slot, buffer, write; } sbuf[16];        // NVN storage slot r -> device [[buffer(n)]]
    uint8_t fout_mask, fout_type[8];                         // colour outputs written
    uint8_t drv_read;                                        // bit n: reads driver constant segment c[n] (0 or 2) at run time
    int depth_minus_one;                                     // vertex: 1 = built with the NEAR_IS_MINUS_W remap, 0 = without, -1 = not stated
    // Compute: threadgroup size (0 = not stated), shared memory bytes, uniform slots read through their GPU address (device
    // [[buffer(n)]]), images (NVN image slot r -> [[texture(n)]], access bit 1 read, bit 2 write).
    uint32_t local[3], shared;
    int nuptr; struct { uint8_t slot, buffer; } uptr[14];
    int nimg; struct { uint8_t slot, texture, type, access; } img[8];
};
// 0 = parsed; otherwise non-zero with the reason in err.
int wonder_shader_refl_parse(const char *text, size_t size, struct wonder_shader_refl *out, char *err, size_t errlen);
