#pragma once
#include "nvn_device.h"
#include "nvn_texture.h"
#define WONDER_NVN_COMMAND_MAGIC UINT32_C(0x57434d44)
struct wonder_nvn_command_header { uint32_t magic, size, opcode, reserved; };
// Typed host records written into guest command memory. Arguments are snapshotted at record time (NVN
// bakes them into the command stream), so later guest writes to the source objects cannot change them.
enum {
    WONDER_NVN_DRAW_ELEMENTS = 1,
    WONDER_NVN_CMD_BIND_BLEND, WONDER_NVN_CMD_BIND_CHANNEL_MASK, WONDER_NVN_CMD_BIND_COLOR, WONDER_NVN_CMD_BIND_DEPTH_STENCIL,
    WONDER_NVN_CMD_BIND_MULTISAMPLE, WONDER_NVN_CMD_BIND_POLYGON, WONDER_NVN_CMD_BIND_VERTEX_ATTRIB, WONDER_NVN_CMD_BIND_VERTEX_STREAM,
    WONDER_NVN_CMD_BIND_PROGRAM, WONDER_NVN_CMD_BIND_VERTEX_BUFFER, WONDER_NVN_CMD_BIND_UNIFORM_BUFFER, WONDER_NVN_CMD_BIND_STORAGE_BUFFER,
    WONDER_NVN_CMD_BIND_TEXTURE, WONDER_NVN_CMD_BIND_TEXTURES, WONDER_NVN_CMD_BIND_IMAGE, WONDER_NVN_CMD_CALL_COMMANDS,
    WONDER_NVN_CMD_CLEAR_BUFFER, WONDER_NVN_CMD_CLEAR_COLOR, WONDER_NVN_CMD_CLEAR_DEPTH_STENCIL, WONDER_NVN_CMD_CLEAR_TEXTURE,
    WONDER_NVN_CMD_COPY_TEXTURE_TO_BUFFER, WONDER_NVN_CMD_COPY_TEXTURE_TO_TEXTURE, WONDER_NVN_CMD_DISPATCH, WONDER_NVN_CMD_DISPATCH_INDIRECT,
    WONDER_NVN_CMD_DOWNSAMPLE, WONDER_NVN_CMD_DRAW_ARRAYS, WONDER_NVN_CMD_DRAW_ARRAYS_INSTANCED, WONDER_NVN_CMD_DRAW_ELEMENTS_BASE_VERTEX,
    WONDER_NVN_CMD_DRAW_ELEMENTS_INDIRECT, WONDER_NVN_CMD_DRAW_ELEMENTS_INSTANCED, WONDER_NVN_CMD_REPORT_COUNTER, WONDER_NVN_CMD_REPORT_VALUE,
    WONDER_NVN_CMD_RESET_COUNTER, WONDER_NVN_CMD_SET_ALPHA_REF, WONDER_NVN_CMD_SET_BLEND_COLOR, WONDER_NVN_CMD_SET_COLOR_REDUCTION_ENABLE,
    WONDER_NVN_CMD_SET_COLOR_REDUCTION_THRESHOLDS, WONDER_NVN_CMD_SET_CONSERVATIVE_RASTER, WONDER_NVN_CMD_SET_DEPTH_BOUNDS,
    WONDER_NVN_CMD_SET_DEPTH_CLAMP, WONDER_NVN_CMD_SET_DEPTH_RANGES, WONDER_NVN_CMD_SET_LINE_WIDTH, WONDER_NVN_CMD_SET_PATCH_SIZE,
    WONDER_NVN_CMD_SET_POINT_SIZE, WONDER_NVN_CMD_SET_POLYGON_OFFSET_CLAMP, WONDER_NVN_CMD_SET_PRIMITIVE_RESTART,
    WONDER_NVN_CMD_SET_RASTERIZER_DISCARD, WONDER_NVN_CMD_SET_RENDER_ENABLE, WONDER_NVN_CMD_SET_RENDER_ENABLE_CONDITIONAL,
    WONDER_NVN_CMD_SET_RENDER_TARGETS, WONDER_NVN_CMD_SET_SAMPLE_MASK, WONDER_NVN_CMD_SET_SAMPLER_POOL, WONDER_NVN_CMD_SET_TEXTURE_POOL,
    WONDER_NVN_CMD_SET_SCISSORS, WONDER_NVN_CMD_SET_SHADER_SCRATCH, WONDER_NVN_CMD_SET_STENCIL_MASK, WONDER_NVN_CMD_SET_STENCIL_REF,
    WONDER_NVN_CMD_SET_STENCIL_VALUE_MASK, WONDER_NVN_CMD_SET_TILED_CACHE_ACTION, WONDER_NVN_CMD_SET_TILED_CACHE_TILE_SIZE,
    WONDER_NVN_CMD_SET_VIEWPORTS, WONDER_NVN_CMD_UPDATE_UNIFORM_BUFFER, WONDER_NVN_CMD_BARRIER,
    WONDER_NVN_CMD_COUNT
};
const char *wonder_nvn_command_name(unsigned opcode);
struct wonder_nvn_draw_elements { int32_t primitive, index_type, count, reserved; uint64_t index_address; };
// Generic record payload: nargs 64-bit arguments (floats stored as their 32-bit pattern), then blob bytes.
struct wonder_nvn_cmd { uint32_t nargs, blob_size; uint64_t a[]; };
static inline const uint8_t *wonder_nvn_cmd_blob(const struct wonder_nvn_cmd *c) { return (const uint8_t *)(c->a + c->nargs); }
// Render-target / copy snapshots stored in blobs.
struct wonder_nvn_texture_snapshot { struct wonder_nvn_texture texture; struct wonder_nvn_texture_view view; uint8_t has_view, valid, pad[6]; };
struct wonder_nvn_copy_region { int32_t x, y, z, width, height, depth; };
// The callback consumes an immutable-by-contract snapshot for this submission; record bytes live in
// caller-owned NVN command memory. Returns no success for unknown commands or native inline packets.
typedef void (*wonder_nvn_command_visitor)(unsigned opcode, const void *payload, size_t size, void *context);
void wonder_nvn_command_walk(uint64_t handle, wonder_nvn_command_visitor visitor, void *context);
int wonder_nvn_command_handle_valid(uint64_t handle);
