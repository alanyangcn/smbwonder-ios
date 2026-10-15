// NVN queue submission for Wonder: replays recorded command handles onto the host Metal queue.
// Executed here: render-target clears, buffer fills, uniform updates, timestamp/value reports, barriers and
// all state tracking (persistent per queue, as GPU state is). Not executed yet: draws, dispatches, texture
// copies/downsamples/clears, non-timestamp counters (TIMESTAMP and TIMESTAMP_TOP run), conditional rendering: replay stops by name before any
// of them is skipped or faked. Host backend: host_graphics.m (device) or the test harness.
#include "nvn_commands.h"
#include "nvn_draw_census.h"
#include "nvn_fmt_mtl.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct replay_state {
    struct wonder_nvn_texture_snapshot targets[9]; int color_count; int has_targets;
    int scissor[4]; int scissor_set; float viewport[4]; int viewport_set;
    int render_enable;
    unsigned submits, commands, clears;
};
static struct replay_state state = {.render_enable = 1};
// Bound program, vertex layout, buffers, textures and fixed-function state, kept for draws (and the draw census).
static struct wonder_nvn_draw_state draw_state;
// One submitter at a time (Wonder submits from its render thread). An owner flag instead of a mutex keeps a
// stopped replay from wedging later submissions on the same thread (tests resubmit after expected stops).
static _Atomic(pthread_t *) replay_owner;
static pthread_t replay_owner_storage[1];
static const char *replay_queue;   // for messages

static float f32(uint64_t bits) { uint32_t u = (uint32_t)bits; float f; memcpy(&f, &u, 4); return f; }
_Noreturn static void replay_stop(unsigned opcode, const char *why) {
    wonder_nvn_stop("NVN_REPLAY_PENDING %s: %s (queue=%s, command %u of submit %u)", wonder_nvn_command_name(opcode), why,
                    replay_queue ? replay_queue : "?", state.commands, state.submits);
}
// Host render target for a snapshot: display textures own theirs, other targets are created by serial.
static void *target_texture(const struct wonder_nvn_texture_snapshot *s, unsigned opcode) {
    const struct wonder_nvn_texture *t = &s->texture;
    if (t->desc.target != 1 && t->desc.target != 4 && t->desc.target != 7)   // NVN 2D, 2D_ARRAY, RECTANGLE
        replay_stop(opcode, "render target is not 2D/2D-array");
    uint32_t mtl = 0; int fit = nvn_fmt_to_mtl(t->desc.format, &mtl, NULL);
    int depth = t->desc.format >= 0x31 && t->desc.format <= 0x36;
    if (!(fit == 1 || (fit == 2 && depth))) replay_stop(opcode, "render target format has no exact Metal render format");
    void *host = wonder_host_render_target(t, mtl);
    if (!host) replay_stop(opcode, "host render target creation failed");
    return host;
}
static void view_level_slice(const struct wonder_nvn_texture_snapshot *s, int *level, int *slice) {
    *level = s->has_view && s->view.set_levels ? s->view.base_level : 0;
    *slice = s->has_view && s->view.set_layers ? s->view.min_layer : 0;
}
// Clears follow the scissor in NVN; only a scissor covering the whole target is executed as a Metal load-clear.
static int scissor_covers(const struct wonder_nvn_texture *t) {
    if (!state.scissor_set) return 1;
    return state.scissor[0] <= 0 && state.scissor[1] <= 0 &&
           (int64_t)state.scissor[0] + state.scissor[2] >= (int64_t)t->desc.width && (int64_t)state.scissor[1] + state.scissor[3] >= (int64_t)t->desc.height;
}

struct report { uint64_t address; uint32_t value; int timestamp; };
static void write_report(void *arg) {
    struct report *r = arg; struct wonder_nvn_pool_view view;
    wonder_nvn_address_span(r->address, r->timestamp ? 16 : 4, &view);
    if (r->timestamp) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t data[2] = {0, (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec};   // NVNcounterData {counter, timestamp ns}
        memcpy(view.memory, data, sizeof data);
    } else memcpy(view.memory, &r->value, 4);
    free(r);
}
static void host_span(uint64_t address, uint64_t size, unsigned opcode, void **buffer, size_t *offset) {
    struct wonder_nvn_pool_view view; wonder_nvn_address_span(address, size, &view);
    if (view.size < size) replay_stop(opcode, "range crosses a virtual mapping boundary");
    if (!view.host_buffer) replay_stop(opcode, "pool has no host GPU view");
    *buffer = view.host_buffer; *offset = view.host_offset;
}

// Draws stop until shaders are translated; the first draw with a program writes the census the offline build needs.
_Noreturn static void draw_stop(unsigned opcode, struct wonder_nvn_draw_call d) {
    d.name = wonder_nvn_command_name(opcode);
    for (int i = 0; i <= state.color_count && i < 9; i++) draw_state.targets[i] = state.targets[i];
    draw_state.color_count = state.color_count; draw_state.has_targets = state.has_targets;
    memcpy(draw_state.viewport, state.viewport, sizeof state.viewport); draw_state.viewport_set = state.viewport_set;
    memcpy(draw_state.scissor, state.scissor, sizeof state.scissor); draw_state.scissor_set = state.scissor_set;
    char summary[160]; wonder_nvn_draw_census(&draw_state, &d, summary, sizeof summary);
    char why[256]; snprintf(why, sizeof why, "draw execution needs translated shaders and PSOs; %s", summary);
    replay_stop(opcode, why);
}
static void replay_visit(unsigned opcode, const void *payload, size_t size, void *context);
static void replay_handle(uint64_t handle, unsigned depth) {
    if (depth > 8) wonder_nvn_stop("NVN_INVALID_COMMAND CallCommands nesting deeper than 8");
    wonder_nvn_command_walk(handle, replay_visit, (void *)(uintptr_t)depth);
}
static void replay_visit(unsigned opcode, const void *payload, size_t size, void *context) {
    (void)size; state.commands++;
    if (opcode == WONDER_NVN_DRAW_ELEMENTS) {
        const struct wonder_nvn_draw_elements *e = payload;
        draw_stop(opcode, (struct wonder_nvn_draw_call){.primitive = e->primitive, .index_type = e->index_type, .count = e->count, .instances = 1, .index_address = e->index_address});
    }
    const struct wonder_nvn_cmd *c = payload; const uint8_t *blob = wonder_nvn_cmd_blob(c);
    switch (opcode) {
    case WONDER_NVN_CMD_CALL_COMMANDS: {
        uint64_t handles[1024]; uint64_t n = c->a[0]; memcpy(handles, blob, n * 8);
        for (uint64_t i = 0; i < n; i++) replay_handle(handles[i], (unsigned)(uintptr_t)context + 1);
        break;
    }
    case WONDER_NVN_CMD_SET_RENDER_TARGETS:
        state.color_count = (int)c->a[0]; memcpy(state.targets, blob, sizeof state.targets[0] * (size_t)(state.color_count + 1));
        state.has_targets = 1;
        for (int i = 0; i <= state.color_count; i++) if (state.targets[i].valid) target_texture(&state.targets[i], opcode);
        break;
    case WONDER_NVN_CMD_SET_SCISSORS:
        if (c->a[0] == 0 && c->a[1] >= 1) { memcpy(state.scissor, blob, 16); state.scissor_set = 1; }
        break;
    case WONDER_NVN_CMD_SET_VIEWPORTS:
        if (c->a[0] == 0 && c->a[1] >= 1) { memcpy(state.viewport, blob, 16); state.viewport_set = 1; }
        break;
    case WONDER_NVN_CMD_SET_RENDER_ENABLE: state.render_enable = (int)c->a[0]; break;
    case WONDER_NVN_CMD_SET_RENDER_ENABLE_CONDITIONAL: replay_stop(opcode, "conditional rendering reads GPU counters (not implemented)");
    case WONDER_NVN_CMD_CLEAR_COLOR: {
        if (!state.render_enable) break;
        int index = (int)c->a[0]; unsigned mask = (unsigned)c->a[1];
        if (!state.has_targets || index >= state.color_count || !state.targets[index].valid) replay_stop(opcode, "no color target bound at the index");
        if (!mask) break;
        if (mask != 0xf) replay_stop(opcode, "partial channel-mask clear needs a shader clear");
        const struct wonder_nvn_texture_snapshot *t = &state.targets[index];
        if (!scissor_covers(&t->texture)) replay_stop(opcode, "scissored clear needs a shader clear");
        float rgba[4]; memcpy(rgba, blob, 16); int level, slice; view_level_slice(t, &level, &slice);
        if (wonder_host_clear_color(target_texture(t, opcode), level, slice, rgba)) replay_stop(opcode, "host clear failed");
        state.clears++;
        break;
    }
    case WONDER_NVN_CMD_CLEAR_DEPTH_STENCIL: {
        if (!state.render_enable) break;
        const struct wonder_nvn_texture_snapshot *t = state.has_targets ? &state.targets[state.color_count] : NULL;
        if (!t || !t->valid) replay_stop(opcode, "no depth target bound");
        int clear_depth = (int)c->a[1] != 0; unsigned stencil_mask = (unsigned)c->a[3] & 0xff;
        int has_stencil = t->texture.desc.format == 0x31 || t->texture.desc.format == 0x35 || t->texture.desc.format == 0x36;
        if (has_stencil && stencil_mask && stencil_mask != 0xff) replay_stop(opcode, "partial stencil-mask clear needs a shader clear");
        if (!scissor_covers(&t->texture)) replay_stop(opcode, "scissored clear needs a shader clear");
        int clear_stencil = has_stencil && stencil_mask == 0xff;
        if (!clear_depth && !clear_stencil) break;
        int level, slice; view_level_slice(t, &level, &slice);
        if (wonder_host_clear_depth_stencil(target_texture(t, opcode), level, slice, clear_depth, f32(c->a[0]), clear_stencil, (uint32_t)c->a[2] & 0xff))
            replay_stop(opcode, "host clear failed");
        state.clears++;
        break;
    }
    case WONDER_NVN_CMD_CLEAR_BUFFER: {
        uint32_t value = (uint32_t)c->a[2]; uint8_t b = (uint8_t)value;
        if (value != b * 0x01010101u) replay_stop(opcode, "non-byte-repeating fill value needs a compute fill");
        void *buffer; size_t offset; host_span(c->a[0], c->a[1], opcode, &buffer, &offset);
        if (wonder_host_fill_buffer(buffer, offset, (size_t)c->a[1], b)) replay_stop(opcode, "host fill failed");
        break;
    }
    case WONDER_NVN_CMD_UPDATE_UNIFORM_BUFFER: {
        uint64_t address = c->a[0] + c->a[2]; size_t bytes = c->blob_size;
        if (!bytes) break;
        void *buffer; size_t offset; host_span(address, bytes, opcode, &buffer, &offset);
        if (wonder_host_write_buffer(buffer, offset, blob, bytes)) replay_stop(opcode, "host buffer update failed");
        break;
    }
    case WONDER_NVN_CMD_REPORT_COUNTER: case WONDER_NVN_CMD_REPORT_VALUE: {
        int timestamp = opcode == WONDER_NVN_CMD_REPORT_COUNTER;
        // TIMESTAMP (bottom of pipe) and TIMESTAMP_TOP (top of pipe) both read the host clock once preceding work is
        // done; occlusion/statistics/ZCull counters have no host source yet.
        if (timestamp && c->a[0] != 0 && c->a[0] != 0x10) {
            static const char *const names[] = {"TIMESTAMP", "SAMPLES_PASSED", "INPUT_VERTICES", "INPUT_PRIMITIVES",
                "VERTEX_SHADER_INVOCATIONS", "TESS_CONTROL_SHADER_INVOCATIONS", "TESS_EVALUATION_SHADER_INVOCATIONS",
                "GEOMETRY_SHADER_INVOCATIONS", "FRAGMENT_SHADER_INVOCATIONS", "TESS_EVALUATION_SHADER_PRIMITIVES",
                "GEOMETRY_SHADER_PRIMITIVES", "CLIPPER_INPUT_PRIMITIVES", "CLIPPER_OUTPUT_PRIMITIVES",
                "PRIMITIVES_GENERATED", "TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN", "ZCULL_STATS"};
            char why[96]; snprintf(why, sizeof why, "counter type %#llx (%s) is not implemented (timestamps only)",
                                   (unsigned long long)c->a[0], c->a[0] < 16 ? names[c->a[0]] : "?");
            replay_stop(opcode, why);
        }
        struct report *r = malloc(sizeof *r);
        if (!r) wonder_nvn_stop("NVN_HOST_OUT_OF_MEMORY report");
        *r = (struct report){c->a[1], timestamp ? 0 : (uint32_t)c->a[0], timestamp};
        wonder_host_after_gpu(write_report, r);
        break;
    }
    case WONDER_NVN_CMD_RESET_COUNTER: replay_stop(opcode, "counter reset (occlusion/statistics counters not implemented)");
    // Draw state (snapshotted at record time; kept for draws and the census).
    case WONDER_NVN_CMD_BIND_PROGRAM:
        draw_state.has_program = c->a[1] != 0;
        memcpy(&draw_state.program, blob, sizeof draw_state.program);
        break;
    case WONDER_NVN_CMD_BIND_VERTEX_ATTRIB:
        draw_state.nattrib = (int)c->a[0]; memcpy(draw_state.attrib, blob, (size_t)draw_state.nattrib * 4); break;
    case WONDER_NVN_CMD_BIND_VERTEX_STREAM:
        draw_state.nstream = (int)c->a[0]; memcpy(draw_state.stream, blob, (size_t)draw_state.nstream * 8); break;
    case WONDER_NVN_CMD_BIND_VERTEX_BUFFER:
        draw_state.vertex_buffer[c->a[0]].address = c->a[1]; draw_state.vertex_buffer[c->a[0]].size = c->a[2]; break;
    case WONDER_NVN_CMD_BIND_UNIFORM_BUFFER:
        draw_state.uniform_buffer[c->a[0]][c->a[1]].address = c->a[2]; draw_state.uniform_buffer[c->a[0]][c->a[1]].size = c->a[3]; break;
    case WONDER_NVN_CMD_BIND_TEXTURE: draw_state.texture_handle[c->a[0]][c->a[1]] = c->a[2]; break;
    case WONDER_NVN_CMD_BIND_TEXTURES: memcpy(&draw_state.texture_handle[c->a[0]][c->a[1]], blob, c->a[2] * 8); break;
    case WONDER_NVN_CMD_SET_TEXTURE_POOL: draw_state.texture_pool = (const void *)(uintptr_t)c->a[0]; break;
    case WONDER_NVN_CMD_SET_SAMPLER_POOL: draw_state.sampler_pool = (const void *)(uintptr_t)c->a[0]; break;
    case WONDER_NVN_CMD_BIND_BLEND: {
        wonder_nvn_blend_state b; memcpy(&b, blob, sizeof b);
        draw_state.blend[b.target] = b; draw_state.blend_set |= (uint8_t)(1u << b.target); break;
    }
    case WONDER_NVN_CMD_BIND_COLOR: memcpy(&draw_state.color, blob, sizeof draw_state.color); draw_state.states_set |= 1; break;
    case WONDER_NVN_CMD_BIND_CHANNEL_MASK: memcpy(&draw_state.channel_mask, blob, sizeof draw_state.channel_mask); draw_state.states_set |= 2; break;
    case WONDER_NVN_CMD_BIND_DEPTH_STENCIL: memcpy(&draw_state.depth_stencil, blob, sizeof draw_state.depth_stencil); draw_state.states_set |= 4; break;
    case WONDER_NVN_CMD_BIND_POLYGON: memcpy(&draw_state.polygon, blob, sizeof draw_state.polygon); draw_state.states_set |= 8; break;
    case WONDER_NVN_CMD_BIND_MULTISAMPLE: memcpy(&draw_state.multisample, blob, sizeof draw_state.multisample); draw_state.states_set |= 16; break;
    // Pure state not consumed by the first draw yet.
    case WONDER_NVN_CMD_BIND_STORAGE_BUFFER: case WONDER_NVN_CMD_BIND_IMAGE:
    case WONDER_NVN_CMD_SET_ALPHA_REF: case WONDER_NVN_CMD_SET_BLEND_COLOR: case WONDER_NVN_CMD_SET_CONSERVATIVE_RASTER:
    case WONDER_NVN_CMD_SET_DEPTH_BOUNDS: case WONDER_NVN_CMD_SET_DEPTH_CLAMP: case WONDER_NVN_CMD_SET_DEPTH_RANGES:
    case WONDER_NVN_CMD_SET_LINE_WIDTH: case WONDER_NVN_CMD_SET_PATCH_SIZE: case WONDER_NVN_CMD_SET_POINT_SIZE:
    case WONDER_NVN_CMD_SET_POLYGON_OFFSET_CLAMP: case WONDER_NVN_CMD_SET_PRIMITIVE_RESTART: case WONDER_NVN_CMD_SET_RASTERIZER_DISCARD:
    case WONDER_NVN_CMD_SET_SAMPLE_MASK:
    case WONDER_NVN_CMD_SET_SHADER_SCRATCH: case WONDER_NVN_CMD_SET_STENCIL_MASK: case WONDER_NVN_CMD_SET_STENCIL_REF:
    case WONDER_NVN_CMD_SET_STENCIL_VALUE_MASK:
        break;
    // TX1 performance hints without visible output; Metal orders hazards on tracked resources itself.
    case WONDER_NVN_CMD_SET_TILED_CACHE_ACTION: case WONDER_NVN_CMD_SET_TILED_CACHE_TILE_SIZE:
    case WONDER_NVN_CMD_SET_COLOR_REDUCTION_ENABLE: case WONDER_NVN_CMD_SET_COLOR_REDUCTION_THRESHOLDS: case WONDER_NVN_CMD_BARRIER:
        break;
    case WONDER_NVN_CMD_DRAW_ARRAYS: case WONDER_NVN_CMD_DRAW_ARRAYS_INSTANCED: case WONDER_NVN_CMD_DRAW_ELEMENTS_BASE_VERTEX:
    case WONDER_NVN_CMD_DRAW_ELEMENTS_INDIRECT: case WONDER_NVN_CMD_DRAW_ELEMENTS_INSTANCED: {
        if (!state.render_enable) break;
        struct wonder_nvn_draw_call d = {.primitive = (int)c->a[0], .instances = 1};
        if (opcode == WONDER_NVN_CMD_DRAW_ARRAYS || opcode == WONDER_NVN_CMD_DRAW_ARRAYS_INSTANCED) {
            d.index_type = -1; d.first = (int)c->a[1]; d.count = (int)c->a[2];
            if (opcode == WONDER_NVN_CMD_DRAW_ARRAYS_INSTANCED) { d.base_instance = (int)c->a[3]; d.instances = (int)c->a[4]; }
        } else if (opcode == WONDER_NVN_CMD_DRAW_ELEMENTS_INDIRECT) {
            d.index_type = (int)c->a[1]; d.index_address = c->a[2]; d.indirect_address = c->a[3];
        } else {
            d.index_type = (int)c->a[1]; d.count = (int)c->a[2]; d.index_address = c->a[3]; d.base_vertex = (int)(int64_t)c->a[4];
            if (opcode == WONDER_NVN_CMD_DRAW_ELEMENTS_INSTANCED) { d.base_instance = (int)c->a[5]; d.instances = (int)c->a[6]; }
        }
        draw_stop(opcode, d);
    }
    case WONDER_NVN_CMD_DISPATCH: case WONDER_NVN_CMD_DISPATCH_INDIRECT:
        replay_stop(opcode, "compute execution needs translated shaders");
    case WONDER_NVN_CMD_CLEAR_TEXTURE: case WONDER_NVN_CMD_COPY_TEXTURE_TO_BUFFER: case WONDER_NVN_CMD_COPY_TEXTURE_TO_TEXTURE:
    case WONDER_NVN_CMD_DOWNSAMPLE:
        replay_stop(opcode, "texture upload/coherence path not implemented");
    default:
        replay_stop(opcode, "unknown command");
    }
}

void wonder_nvnQueueSubmitCommands(void *queue, int count, const uint64_t *handles) {
    wonder_nvn_queue_check(queue, "nvnQueueSubmitCommands");
    if (count < 0 || (count && !handles)) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnQueueSubmitCommands count=%d", count);
    pthread_t *owner = atomic_load(&replay_owner);
    if (owner && !pthread_equal(*owner, pthread_self())) wonder_nvn_stop("NVN_UNSUPPORTED nvnQueueSubmitCommands concurrent submitters");
    replay_owner_storage[0] = pthread_self(); atomic_store(&replay_owner, replay_owner_storage);
    state.submits++; replay_queue = "main";
    unsigned before = state.commands, clears = state.clears;
    for (int i = 0; i < count; i++) {
        if (!wonder_nvn_command_handle_valid(handles[i]))
            wonder_nvn_stop("NVN_INVALID_HANDLE nvnQueueSubmitCommands handle[%d]=%#llx", i, (unsigned long long)handles[i]);
        replay_handle(handles[i], 0);
    }
    unsigned submits = state.submits, commands = state.commands - before; clears = state.clears - clears;
    atomic_store(&replay_owner, (pthread_t *)NULL);
    if (submits <= 8 || submits % 600 == 0)
        wonder_nvn_report("NVN_QUEUE_SUBMIT queue=%p submit=%u handles=%d commands=%u clears=%u", queue, submits, count, commands, clears);
}
void wonder_nvnQueueFlush(void *queue) { wonder_nvn_queue_check(queue, "nvnQueueFlush"); wonder_host_flush(); }
void wonder_nvnQueueFinish(void *queue) { wonder_nvn_queue_check(queue, "nvnQueueFinish"); wonder_host_finish(); }

const struct wonder_nvn_entry wonder_nvn_queue_entries[] = {
    {"nvnQueueSubmitCommands", (void *)wonder_nvnQueueSubmitCommands},
    {"nvnQueueFlush", (void *)wonder_nvnQueueFlush},
    {"nvnQueueFinish", (void *)wonder_nvnQueueFinish},
};
const unsigned wonder_nvn_queue_count = sizeof wonder_nvn_queue_entries / sizeof *wonder_nvn_queue_entries;
