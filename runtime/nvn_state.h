#pragma once
// Host layouts of the NVN state objects (value types packed within NVN's reserved sizes; layouts follow D2R
// dm/gfx/nvn/nvn_obj.h). Shared by nvn_state.c (setters), the replay and the draw census.
#include <stdint.h>
typedef struct { uint64_t target : 3, src_rgb : 7, dst_rgb : 7, src_a : 7, dst_a : 7, eq_rgb : 3, eq_a : 3; } wonder_nvn_blend_state;   // 8
typedef struct { uint32_t blend_en : 8, logic_op : 4, alpha_func : 4, alpha_ref_bits : 16; } wonder_nvn_color_state;                       // 4
typedef struct { uint32_t mask; } wonder_nvn_channel_mask_state;                                                                          // 4
typedef struct { uint8_t enable, samples, a2c, a2c_dither, raster_samples, cov_mod, cov2color_en, cov2color_out, sl_en, sl_grid; } wonder_nvn_multisample_state;   // 0x18
typedef struct { uint32_t cull : 2, front : 1, mode : 2, offset_en : 3; } wonder_nvn_polygon_state;                                       // 4
typedef struct { uint64_t dt : 1, dw : 1, df : 4, st : 1, sf_f : 4, sf_b : 4, fail_f : 4, zfail_f : 4, zpass_f : 4, fail_b : 4, zfail_b : 4, zpass_b : 4; } wonder_nvn_depth_stencil_state;   // 8
typedef struct { uint32_t fmt : 8, offset : 19, stream : 5; } wonder_nvn_vertex_attrib_state;                                             // 4
typedef struct { uint32_t stride, divisor; } wonder_nvn_vertex_stream_state;                                                              // 8
_Static_assert(sizeof(wonder_nvn_blend_state) <= 8 && sizeof(wonder_nvn_color_state) <= 4 && sizeof(wonder_nvn_channel_mask_state) <= 4 &&
               sizeof(wonder_nvn_multisample_state) <= 0x18 && sizeof(wonder_nvn_polygon_state) <= 4 && sizeof(wonder_nvn_depth_stencil_state) <= 8 &&
               sizeof(wonder_nvn_vertex_attrib_state) <= 4 && sizeof(wonder_nvn_vertex_stream_state) <= 8, "NVN state reserved sizes");
