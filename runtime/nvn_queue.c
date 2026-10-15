// NVN queue submission for Wonder: replays recorded command handles onto the host Metal queue.
// Executed here: render-target clears, buffer fills, uniform updates, timestamp/value reports, barriers and
// all state tracking (persistent per queue, as GPU state is). Not executed yet: draws, dispatches, texture
// copies/downsamples/clears, non-timestamp counters (TIMESTAMP and TIMESTAMP_TOP run), conditional rendering: replay stops by name before any
// of them is skipped or faked. Host backend: host_graphics.m (device) or the test harness.
#include "nvn_commands.h"
#include "nvn_tex.h"
#include "nvn_draw_census.h"
#include "host_draw.h"
#include "nvn_fmt_mtl.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct replay_state {
    struct wonder_nvn_texture_snapshot targets[9]; int color_count; int has_targets;
    int scissor[4]; int scissor_set; float viewport[4]; int viewport_set; float depth_range[2]; float blend_color[4];
    int render_enable;
    unsigned submits, commands, clears;
};
static struct replay_state state = {.render_enable = 1, .depth_range = {0.0f, 1.0f}};
// Per-present summary (diagnostic: compares good and bad frames): draws encoded since the last present, their
// program/count sequence hash, and clears.
static _Atomic unsigned frame_draws; static _Atomic uint64_t frame_hash = 1469598103934665603ull;
void wonder_nvn_frame_summary(unsigned *draws, unsigned *clears, uint64_t *hash) {
    static unsigned clears_at_last; unsigned c = state.clears;
    *draws = atomic_exchange(&frame_draws, 0); *hash = atomic_exchange(&frame_hash, 1469598103934665603ull);
    *clears = c - clears_at_last; clears_at_last = c;
}
// Bound program, vertex layout, buffers, textures and fixed-function state, kept for draws (and the draw census).
static struct wonder_nvn_draw_state draw_state;
// UpdateUniformBuffer ranges recorded since the replay last finished the GPU (GPU-ordered writes the CPU cannot see yet,
// including earlier submits still pending). Reading such memory on the CPU first finishes the work recorded so far.
static struct { uint64_t address, size; } ubo_updates[64]; static unsigned ubo_update_count, ubo_update_overflow;
static void make_cpu_visible(uint64_t address, uint64_t size) {
    int overlap = ubo_update_overflow;
    for (unsigned i = 0; !overlap && i < ubo_update_count; i++) overlap = address < ubo_updates[i].address + ubo_updates[i].size && ubo_updates[i].address < address + size;
    if (!overlap) return;
    wonder_host_finish();
    ubo_update_count = 0; ubo_update_overflow = 0;
}
// Rasterizer state the Metal path cannot honour except at its NVN defaults (refused at draw time when changed).
static struct { int rasterizer_discard, depth_clamp, restart, restart_index; uint32_t sample_mask; float line_width; unsigned program_stages; }
    raster = {.sample_mask = 0xffffffffu, .line_width = 1.0f};
// Census-only: after the first draw whose program has no translated shaders, the rest of the submit only tracks state and
// writes a census for every further missing program; no GPU work of this submit runs after that point, and the submit
// stops by name at its end (nothing is presented from a partly executed frame).
static int census_only; static unsigned missing_programs; static char missing_list[512];
static char missing_pairs[256][34];   // dedup independent of the (truncated) message list
// Per-submit draw summary for the log (which programs a failing command buffer contained).
static unsigned submit_draws; static char submit_programs[256];
// The first executed draw's colour target is read back (evidence) after the submit that holds it.
static struct wonder_nvn_texture_snapshot first_draw_target; static int first_draw_pending, first_draw_captured;
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
    if (t->desc.target != 1 && t->desc.target != 2 && t->desc.target != 4 && t->desc.target != 7 && t->desc.target != 8 && t->desc.target != 9) {   // 2D, 3D, 2D_ARRAY, RECTANGLE, CUBE, CUBE_ARRAY
        char why[160]; snprintf(why, sizeof why, "render target target %d format %#x %ux%ux%u samples %d is not supported", t->desc.target, t->desc.format,
                                t->desc.width, t->desc.height, t->desc.depth, t->desc.samples);
        replay_stop(opcode, why);
    }
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

// Draws: resolve the bound program to translated shaders and every buffer/texture its reflection reads, then hand the
// host one fully resolved draw. A program without translated shaders writes the census (once per run) and stops.
static void sync_draw_state(void) {
    for (int i = 0; i <= state.color_count && i < 9; i++) draw_state.targets[i] = state.targets[i];
    draw_state.color_count = state.color_count; draw_state.has_targets = state.has_targets;
    memcpy(draw_state.viewport, state.viewport, sizeof state.viewport); draw_state.viewport_set = state.viewport_set;
    memcpy(draw_state.scissor, state.scissor, sizeof state.scissor); draw_state.scissor_set = state.scissor_set;
}
static struct wonder_host_span span_of(unsigned opcode, uint64_t address, uint64_t size) {
    struct wonder_host_span s = {0}; host_span(address, size, opcode, &s.buffer, &s.offset); s.size = (size_t)size; return s;
}
static void draw(unsigned opcode, struct wonder_nvn_draw_call call) {
    call.name = wonder_nvn_command_name(opcode);
    sync_draw_state();
    char why[320], summary[160];
    if (!draw_state.has_program || (draw_state.program.mask & 3) != 3 || (draw_state.program.mask & ~3u)) {
        wonder_nvn_draw_census(&draw_state, &call, summary, sizeof summary);
        snprintf(why, sizeof why, "draw needs a vertex+fragment program (stages %#x); %s", draw_state.has_program ? draw_state.program.mask : 0, summary);
        replay_stop(opcode, why);
    }
    if ((raster.program_stages & 0x11) != 0x11) { snprintf(why, sizeof why, "program stage bits %#x do not bind both vertex and fragment", raster.program_stages); replay_stop(opcode, why); }
    if (raster.rasterizer_discard) replay_stop(opcode, "rasterizer discard is enabled (not executed yet)");
    if (raster.depth_clamp) replay_stop(opcode, "depth clamp is enabled (not executed yet)");
    if (raster.sample_mask != 0xffffffffu) { snprintf(why, sizeof why, "sample mask %#x is not all samples", raster.sample_mask); replay_stop(opcode, why); }
    if ((call.primitive == 1 || call.primitive == 2 || call.primitive == 3) && raster.line_width != 1.0f) replay_stop(opcode, "line width other than 1 (Metal draws 1-pixel lines)");
    if ((draw_state.states_set & 8) && draw_state.polygon.offset_en) replay_stop(opcode, "polygon offset (depth bias) is not executed yet");
    int depth_mode, origin; wonder_nvn_device_modes(&depth_mode, &origin);
    if (origin != 1) { snprintf(why, sizeof why, "window origin %d (only UPPER_LEFT matches the translated shaders)", origin); replay_stop(opcode, why); }
    static struct wonder_host_draw d; memset(&d, 0, sizeof d);
    wonder_nvn_shader_key(draw_state.program.control[0], d.vs_key); wonder_nvn_shader_key(draw_state.program.control[1], d.fs_key);
    if (!wonder_host_shader_available(d.vs_key) || !wonder_host_shader_available(d.fs_key)) {
        wonder_nvn_draw_census(&draw_state, &call, summary, sizeof summary);
        char pair[40]; snprintf(pair, sizeof pair, "%s-%s", d.vs_key, d.fs_key);
        int seen = 0;
        for (unsigned i = 0; i < missing_programs && i < 256; i++) if (!strcmp(missing_pairs[i], pair)) { seen = 1; break; }
        if (!seen) {
            if (missing_programs < 256) snprintf(missing_pairs[missing_programs], sizeof missing_pairs[0], "%s", pair);
            size_t used = strlen(missing_list);
            if (used + strlen(pair) + 2 < sizeof missing_list) snprintf(missing_list + used, sizeof missing_list - used, "%s%s", used ? " " : "", pair);
            missing_programs++;
        }
        if (!census_only) wonder_nvn_report("NVN_CENSUS_ONLY from command %u of submit %u: %s", state.commands, state.submits, summary);
        census_only = 1;
        return;
    }
    if (census_only) return;   // translated, but this submit no longer executes
    d.vr = wonder_host_shader_refl(d.vs_key); d.fr = wonder_host_shader_refl(d.fs_key);
    if (d.vr->stage != 0 || d.fr->stage != 1) replay_stop(opcode, "shader reflection stage does not match the program stage");
    int want_minus_one = depth_mode != 1;   // NVN default (never set, -1) is NEAR_IS_MINUS_W
    if (d.vr->depth_minus_one != want_minus_one) {
        snprintf(why, sizeof why, "depth mode: device %d but vertex shader %s was built %s", depth_mode, d.vs_key,
                 d.vr->depth_minus_one < 0 ? "without a depth line" : d.vr->depth_minus_one ? "for NEAR_IS_MINUS_W" : "for NEAR_IS_ZERO");
        replay_stop(opcode, why);
    }
    d.vs = wonder_host_shader_function(d.vs_key, 0); d.fs = wonder_host_shader_function(d.fs_key, 1);
    const struct wonder_shader_refl *refl[2] = {d.vr, d.fr};
    for (int stage = 0; stage < 2; stage++) {
        const struct wonder_shader_refl *r = refl[stage];
        if (r->drv_read) { snprintf(why, sizeof why, "stage %d reads a driver constant segment (c0/c2 mask %#x) at run time", stage, r->drv_read); replay_stop(opcode, why); }
        for (int i = 0; i < r->ncbuf; i++) {
            int slot = r->cbuf[i].slot;
            if (!draw_state.uniform_buffer[stage][slot].address) { snprintf(why, sizeof why, "unbound uniform buffer stage %d slot %d", stage, slot); replay_stop(opcode, why); }
            d.ubo[stage][slot] = span_of(opcode, draw_state.uniform_buffer[stage][slot].address, draw_state.uniform_buffer[stage][slot].size);
        }
        for (int i = 0; i < r->nsbuf; i++) {
            int slot = r->sbuf[i].slot;
            if (!draw_state.storage_buffer[stage][slot].address || !draw_state.storage_buffer[stage][slot].size) {
                snprintf(why, sizeof why, "unbound storage buffer stage %d slot %d", stage, slot); replay_stop(opcode, why); }
            d.sbuf[stage][slot] = span_of(opcode, draw_state.storage_buffer[stage][slot].address, draw_state.storage_buffer[stage][slot].size);
        }
        for (int i = 0; i < r->ntex; i++) {
            int slot = r->tex[i].slot; uint64_t h;
            if (r->tex[i].bindless) {   // combined handle stored by the game in a uniform buffer (round86)
                uint64_t base = draw_state.uniform_buffer[stage][r->tex[i].ubo_slot].address, size = draw_state.uniform_buffer[stage][r->tex[i].ubo_slot].size;
                if (!base || r->tex[i].offset + 4u > size) { snprintf(why, sizeof why, "bindless texture handle: uniform slot %d not bound or too small (offset %#x)", r->tex[i].ubo_slot, r->tex[i].offset); replay_stop(opcode, why); }
                make_cpu_visible(base + r->tex[i].offset, 4);   // round87: the handle was written by UpdateUniformBuffer in this submit
                struct wonder_nvn_pool_view v; wonder_nvn_address_span(base + r->tex[i].offset, 4, &v); uint32_t raw; memcpy(&raw, v.memory, 4); h = raw;
            } else h = draw_state.texture_handle[stage][slot];
            const struct wonder_nvn_texture_registration *t = h && draw_state.texture_pool ? wonder_nvn_texture_lookup(draw_state.texture_pool, (int)(h & 0xfffff)) : NULL;
            const struct wonder_nvn_sampler_state *m = h && draw_state.sampler_pool ? wonder_nvn_sampler_lookup(draw_state.sampler_pool, (int)((h >> 20) & 0xfff)) : NULL;
            if (!t || !t->valid || !m || !m->valid) { snprintf(why, sizeof why, "unbound texture stage %d slot %d (handle %#llx)", stage, slot, (unsigned long long)h); replay_stop(opcode, why); }
            // NVN target (1D 0, 2D 1, 3D 2, 1D_ARRAY 3, 2D_ARRAY 4, 2DMS 5, 2DMS_ARRAY 6, RECTANGLE 7, CUBE 8, CUBE_ARRAY 9, BUFFER 10) -> reflection type.
            static const int target_type[11] = {WONDER_SHADER_TEX_1D, WONDER_SHADER_TEX_2D, WONDER_SHADER_TEX_3D, WONDER_SHADER_TEX_1D_ARRAY, WONDER_SHADER_TEX_2D_ARRAY,
                WONDER_SHADER_TEX_2D_MS, WONDER_SHADER_TEX_2D_MS_ARRAY, WONDER_SHADER_TEX_2D, WONDER_SHADER_TEX_CUBE, WONDER_SHADER_TEX_CUBE_ARRAY, WONDER_SHADER_TEX_BUFFER};
            int target = t->has_view && t->view.set_target ? t->view.target : t->texture.desc.target;
            if (target < 0 || target > 10 || target_type[target] != r->tex[i].type) {
                snprintf(why, sizeof why, "texture type: stage %d slot %d bound target %d, shader declares type %d", stage, slot, target, r->tex[i].type); replay_stop(opcode, why);
            }
            d.tex[stage][slot].texture = *t; d.tex[stage][slot].sampler = *m; d.tex_mask[stage] |= 1ull << slot;
            if (!t->texture.host) {   // display textures are GPU-only; others are uploaded from their pool storage
                struct wonder_nvn_pool_view v; wonder_nvn_pool_span(t->texture.pool, (uint64_t)t->texture.offset, t->texture.size, &v);
                if (v.size < t->texture.size || !v.memory) { snprintf(why, sizeof why, "texture stage %d slot %d storage is not CPU-resolvable", stage, slot); replay_stop(opcode, why); }
                d.tex[stage][slot].storage = v.memory; d.tex[stage][slot].storage_size = t->texture.size;
            }
        }
    }
    for (int i = 0; i < d.vr->nattr; i++) {
        int a = d.vr->attr[i].nvn;
        if (a >= draw_state.nattrib || !draw_state.attrib[a].fmt) {   // disabled: Maxwell reads (0,0,0,1) (round92)
            d.attr[i].format = 0; d.attr[i].offset = 0; d.attr[i].stream = WONDER_HOST_DEFAULT_ATTRIBUTE; continue;
        }
        int s = (int)draw_state.attrib[a].stream;
        if (s >= 15) { snprintf(why, sizeof why, "vertex stream %d would need [[buffer(%d)]] (Metal has 0..30)", s, 16 + s); replay_stop(opcode, why); }
        d.attr[i].format = draw_state.attrib[a].fmt; d.attr[i].offset = draw_state.attrib[a].offset; d.attr[i].stream = (uint32_t)s;
        if (!(d.stream_mask & (1u << s))) {
            if (s >= draw_state.nstream || !draw_state.vertex_buffer[s].address) { snprintf(why, sizeof why, "unbound vertex buffer stream %d", s); replay_stop(opcode, why); }
            d.stream[s].span = span_of(opcode, draw_state.vertex_buffer[s].address, draw_state.vertex_buffer[s].size);
            d.stream[s].stride = draw_state.stream[s].stride; d.stream[s].divisor = draw_state.stream[s].divisor; d.stream_mask |= (uint16_t)(1u << s);
        }
    }
    d.nattr = d.vr->nattr;
    d.primitive = call.primitive; d.index_type = call.index_type; d.count = call.count; d.first = call.first;
    d.base_vertex = call.base_vertex; d.instances = call.instances; d.base_instance = call.base_instance;
    if (call.indirect_address) replay_stop(opcode, "indirect draws are not executed yet");
    if (call.index_type >= 0) {
        d.index = span_of(opcode, call.index_address, (uint64_t)call.count << call.index_type);
        // Metal restarts strips on the all-ones index and never restarts lists; NVN restarts on its own index when enabled.
        uint32_t all_ones = call.index_type == 0 ? 0xffu : call.index_type == 1 ? 0xffffu : 0xffffffffu;
        int strip = call.primitive == 3 || call.primitive == 5;
        if (strip && !(raster.restart && (uint32_t)raster.restart_index == all_ones)) replay_stop(opcode, "strip draw without NVN primitive restart on the all-ones index (Metal always restarts there)");
        if (!strip && raster.restart) {
            struct wonder_nvn_pool_view v; wonder_nvn_address_span(call.index_address, (uint64_t)call.count << call.index_type, &v);
            for (int i = 0; i < call.count; i++) {
                uint32_t x = call.index_type == 0 ? v.memory[i] : call.index_type == 1 ? ((const uint16_t *)v.memory)[i] : ((const uint32_t *)v.memory)[i];
                if (x == (uint32_t)raster.restart_index) { snprintf(why, sizeof why, "primitive restart index %#x appears in a list draw", raster.restart_index); replay_stop(opcode, why); }
            }
        }
    }
    // Vertex fetch range: NVN bounds each stream by its bound size; Metal would read past the pool's buffer (round62: GPU
    // address fault). Indices are scanned on the CPU (they are in pool memory) for the highest vertex.
    if (d.nattr && call.count > 0) {
        int64_t max_vertex;
        if (call.index_type >= 0) {
            struct wonder_nvn_pool_view v; wonder_nvn_address_span(call.index_address, (uint64_t)call.count << call.index_type, &v);
            uint32_t top = 0;
            for (int i = 0; i < call.count; i++) {
                uint32_t x = call.index_type == 0 ? v.memory[i] : call.index_type == 1 ? ((const uint16_t *)v.memory)[i] : ((const uint32_t *)v.memory)[i];
                if (raster.restart && x == (uint32_t)raster.restart_index) continue;
                if (x > top) top = x;
            }
            max_vertex = (int64_t)top + call.base_vertex;
        } else max_vertex = (int64_t)call.first + call.count - 1;
        int64_t max_instance = (int64_t)call.base_instance + (call.instances > 0 ? call.instances - 1 : 0);
        for (int i = 0; i < d.nattr; i++) {
            if (d.attr[i].stream == WONDER_HOST_DEFAULT_ATTRIBUTE) continue;
            int s = (int)d.attr[i].stream, bw, bh, bytes = nvn_fmt_bpb((int)d.attr[i].format, &bw, &bh);
            uint32_t stride = d.stream[s].stride, divisor = d.stream[s].divisor;
            int64_t element = !stride ? 0 : divisor ? max_instance / divisor : max_vertex;
            uint64_t end = (uint64_t)element * stride + d.attr[i].offset + (uint64_t)(bytes > 0 ? bytes : 1);
            // Past the bound size Maxwell returns zeros (round82: a 6-index draw over 3 bound vertices); the host zero-pads
            // the stream to the fetched size. Negative vertex indices and absurd sizes stop.
            if (max_vertex < 0 || end > (64u << 20)) {
                snprintf(why, sizeof why, "vertex fetch out of range: stream %d element %lld stride %u attribute offset %u ends at %#llx, bound %#zx",
                         s, (long long)element, stride, d.attr[i].offset, (unsigned long long)end, d.stream[s].span.size);
                replay_stop(opcode, why);
            }
            if (end > d.stream[s].fetch_size) d.stream[s].fetch_size = end;
        }
    }
    for (int i = 0; i < 9; i++) d.targets[i] = draw_state.targets[i];
    d.color_count = draw_state.has_targets ? draw_state.color_count : 0;
    memcpy(d.blend, draw_state.blend, sizeof d.blend); d.blend_set = draw_state.blend_set; memcpy(d.blend_color, state.blend_color, sizeof d.blend_color);
    d.color = draw_state.color; d.channel_mask = draw_state.channel_mask; d.depth_stencil = draw_state.depth_stencil;
    d.polygon = draw_state.polygon; d.multisample = draw_state.multisample; d.states_set = draw_state.states_set;
    memcpy(d.viewport, state.viewport, sizeof d.viewport); d.viewport_set = state.viewport_set;
    memcpy(d.depth_range, state.depth_range, sizeof d.depth_range);
    memcpy(d.scissor, state.scissor, sizeof d.scissor); d.scissor_set = state.scissor_set;
    d.depth_mode = depth_mode; d.window_origin = origin;
    if (wonder_host_draw(&d)) replay_stop(opcode, "host draw encoding failed");
    if (!first_draw_captured && !first_draw_pending)
        for (int i = 0; i < d.color_count; i++) if (d.targets[i].valid) { first_draw_target = d.targets[i]; first_draw_pending = 1; break; }
    submit_draws++;
    { uint64_t h = atomic_load(&frame_hash); for (const char *k = d.vs_key; *k; k++) h = (h ^ (uint8_t)*k) * 1099511628211ull;
      for (const char *k = d.fs_key; *k; k++) h = (h ^ (uint8_t)*k) * 1099511628211ull;
      h = (h ^ (uint32_t)call.count) * 1099511628211ull; atomic_store(&frame_hash, h); atomic_fetch_add(&frame_draws, 1); }
    { char pair[24]; snprintf(pair, sizeof pair, "%.8s-%.8s", d.vs_key, d.fs_key); size_t used = strlen(submit_programs);
      if (!strstr(submit_programs, pair) && used + strlen(pair) + 2 < sizeof submit_programs) snprintf(submit_programs + used, sizeof submit_programs - used, "%s%s", used ? "," : "", pair); }
    static _Atomic unsigned logged;
    if (atomic_fetch_add(&logged, 1) < 16)
        wonder_nvn_report("NVN_DRAW_EXECUTED %s vs=%s fs=%s count=%d instances=%d (submit %u command %u)", call.name, d.vs_key, d.fs_key, call.count, call.instances, state.submits, state.commands);
}
static void replay_visit(unsigned opcode, const void *payload, size_t size, void *context);
static void replay_handle(uint64_t handle, unsigned depth) {
    if (depth > 8) wonder_nvn_stop("NVN_INVALID_COMMAND CallCommands nesting deeper than 8");
    wonder_nvn_command_walk(handle, replay_visit, (void *)(uintptr_t)depth);
}
static void replay_visit(unsigned opcode, const void *payload, size_t size, void *context) {
    (void)size; state.commands++;
    if (census_only) switch (opcode) {   // GPU work after the first missing program does not run in this submit
    case WONDER_NVN_CMD_CLEAR_COLOR: case WONDER_NVN_CMD_CLEAR_DEPTH_STENCIL: case WONDER_NVN_CMD_CLEAR_BUFFER: case WONDER_NVN_CMD_UPDATE_UNIFORM_BUFFER:
    case WONDER_NVN_CMD_REPORT_COUNTER: case WONDER_NVN_CMD_REPORT_VALUE: case WONDER_NVN_CMD_RESET_COUNTER: case WONDER_NVN_CMD_CLEAR_TEXTURE:
    case WONDER_NVN_CMD_COPY_TEXTURE_TO_BUFFER: case WONDER_NVN_CMD_COPY_TEXTURE_TO_TEXTURE: case WONDER_NVN_CMD_DOWNSAMPLE:
    case WONDER_NVN_CMD_DISPATCH: case WONDER_NVN_CMD_DISPATCH_INDIRECT:
        return;
    default: break;
    }
    if (opcode == WONDER_NVN_DRAW_ELEMENTS) {
        const struct wonder_nvn_draw_elements *e = payload;
        if (state.render_enable)
            draw(opcode, (struct wonder_nvn_draw_call){.primitive = e->primitive, .index_type = e->index_type, .count = e->count, .instances = 1, .index_address = e->index_address});
        return;
    }
    const struct wonder_nvn_cmd *c = payload; const uint8_t *blob = wonder_nvn_cmd_blob(c);
    switch (opcode) {
    case WONDER_NVN_CMD_CALL_COMMANDS: {
        uint64_t n = c->a[0];   // handles read from the snapshotted blob (no 8 KiB stack copy per nesting level)
        for (uint64_t i = 0; i < n; i++) { uint64_t h; memcpy(&h, blob + i * 8, 8); replay_handle(h, (unsigned)(uintptr_t)context + 1); }
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
    case WONDER_NVN_CMD_SET_RASTERIZER_DISCARD: raster.rasterizer_discard = c->a[0] != 0; break;
    case WONDER_NVN_CMD_SET_DEPTH_CLAMP: raster.depth_clamp = c->a[0] != 0; break;
    case WONDER_NVN_CMD_SET_SAMPLE_MASK: raster.sample_mask = (uint32_t)c->a[0]; break;
    case WONDER_NVN_CMD_SET_LINE_WIDTH: raster.line_width = f32(c->a[0]); break;
    case WONDER_NVN_CMD_SET_PRIMITIVE_RESTART: raster.restart = c->a[0] != 0; raster.restart_index = (int)(uint32_t)c->a[1]; break;
    case WONDER_NVN_CMD_SET_BLEND_COLOR: memcpy(state.blend_color, blob, 16); break;
    case WONDER_NVN_CMD_SET_DEPTH_RANGES:
        if (c->a[0] == 0 && c->a[1] >= 1) memcpy(state.depth_range, blob, 8);
        break;
    case WONDER_NVN_CMD_SET_RENDER_ENABLE: state.render_enable = (int)c->a[0]; break;
    case WONDER_NVN_CMD_SET_RENDER_ENABLE_CONDITIONAL: {
        // Maxwell compares the 64-bit counters of the two report records at address and address+16 (EQUAL mode 0,
        // NOT_EQUAL mode 1). Metal has no draw predication: finish the GPU work recorded so far (report writes run in
        // its completion handlers), then evaluate on the CPU. A sync point, but exact.
        wonder_host_finish(); ubo_update_count = 0; ubo_update_overflow = 0;
        struct wonder_nvn_pool_view v; wonder_nvn_address_span(c->a[1], 32, &v);
        uint64_t a, b; memcpy(&a, v.memory, 8); memcpy(&b, v.memory + 16, 8);
        state.render_enable = c->a[0] == 0 ? a == b : a != b;
        static _Atomic unsigned logged;
        if (atomic_fetch_add(&logged, 1) < 8)
            wonder_nvn_report("NVN_CONDITIONAL_RENDER mode=%llu counters=%llu,%llu -> %s (submit %u)", (unsigned long long)c->a[0],
                              (unsigned long long)a, (unsigned long long)b, state.render_enable ? "render" : "skip", state.submits);
        break;
    }
    case WONDER_NVN_CMD_CLEAR_COLOR: {
        if (!state.render_enable) break;
        int index = (int)c->a[0]; unsigned mask = (unsigned)c->a[1];
        if (!state.has_targets || index >= state.color_count || !state.targets[index].valid) replay_stop(opcode, "no color target bound at the index");
        if (!mask) break;
        const struct wonder_nvn_texture_snapshot *t = &state.targets[index];
        float rgba[4]; memcpy(rgba, blob, 16); int level, slice; view_level_slice(t, &level, &slice);
        if (mask != 0xf || !scissor_covers(&t->texture)) {   // drawn over the scissor rectangle with the channel mask
            int full[4] = {0, 0, (int)t->texture.desc.width, (int)t->texture.desc.height};
            if (wonder_host_clear_rect(target_texture(t, opcode), level, slice, rgba, mask, state.scissor_set ? state.scissor : full))
                replay_stop(opcode, "scissored/masked clear failed (integer target formats are not drawn yet)");
        } else if (wonder_host_clear_color(target_texture(t, opcode), level, slice, rgba)) replay_stop(opcode, "host clear failed");
        state.clears++;
        break;
    }
    case WONDER_NVN_CMD_CLEAR_DEPTH_STENCIL: {
        if (!state.render_enable) break;
        const struct wonder_nvn_texture_snapshot *t = state.has_targets ? &state.targets[state.color_count] : NULL;
        if (!t || !t->valid) replay_stop(opcode, "no depth target bound");
        int clear_depth = (int)c->a[1] != 0; unsigned stencil_mask = (unsigned)c->a[3] & 0xff;
        int has_stencil = t->texture.desc.format == 0x31 || t->texture.desc.format == 0x35 || t->texture.desc.format == 0x36;
        int level, slice; view_level_slice(t, &level, &slice);
        if ((has_stencil && stencil_mask && stencil_mask != 0xff) || !scissor_covers(&t->texture)) {   // drawn over the scissor rect
            int full[4] = {0, 0, (int)t->texture.desc.width, (int)t->texture.desc.height};
            if (wonder_host_clear_depth_stencil_rect(target_texture(t, opcode), level, slice, clear_depth, f32(c->a[0]), has_stencil && stencil_mask,
                                                     (uint32_t)c->a[2] & 0xff, stencil_mask, state.scissor_set ? state.scissor : full))
                replay_stop(opcode, "scissored/masked depth-stencil clear failed");
            state.clears++;
            break;
        }
        int clear_stencil = has_stencil && stencil_mask == 0xff;
        if (!clear_depth && !clear_stencil) break;
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
        if (ubo_update_count < 64) { ubo_updates[ubo_update_count].address = address; ubo_updates[ubo_update_count++].size = bytes; } else ubo_update_overflow = 1;
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
        raster.program_stages = (unsigned)c->a[0];
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
    case WONDER_NVN_CMD_BIND_STORAGE_BUFFER:
        draw_state.storage_buffer[c->a[0]][c->a[1]].address = c->a[2]; draw_state.storage_buffer[c->a[0]][c->a[1]].size = c->a[3]; break;
    // Pure state not consumed by draws yet.
    case WONDER_NVN_CMD_BIND_IMAGE:
    case WONDER_NVN_CMD_SET_ALPHA_REF: case WONDER_NVN_CMD_SET_CONSERVATIVE_RASTER:
    case WONDER_NVN_CMD_SET_DEPTH_BOUNDS:
    case WONDER_NVN_CMD_SET_PATCH_SIZE: case WONDER_NVN_CMD_SET_POINT_SIZE:
    case WONDER_NVN_CMD_SET_POLYGON_OFFSET_CLAMP:
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
        draw(opcode, d);
        break;
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

struct submit_job { void *queue; int count; const uint64_t *handles; };
static void submit_on_replay_stack(void *arg);
void wonder_nvnQueueSubmitCommands(void *queue, int count, const uint64_t *handles) {
    wonder_nvn_queue_check(queue, "nvnQueueSubmitCommands");
    if (count < 0 || (count && !handles)) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnQueueSubmitCommands count=%d", count);
    pthread_t *owner = atomic_load(&replay_owner);
    if (owner && !pthread_equal(*owner, pthread_self())) wonder_nvn_stop("NVN_UNSUPPORTED nvnQueueSubmitCommands concurrent submitters");
    replay_owner_storage[0] = pthread_self(); atomic_store(&replay_owner, replay_owner_storage);
    struct submit_job job = {queue, count, handles};
    wonder_host_run_on_replay_stack(submit_on_replay_stack, &job);
}
static void submit_on_replay_stack(void *arg) {
    const struct submit_job *job = arg; void *queue = job->queue; int count = job->count; const uint64_t *handles = job->handles;
    char gpu_error[256];
    if (wonder_host_gpu_error(gpu_error, sizeof gpu_error))
        wonder_nvn_stop("NVN_GPU_ERROR an earlier replay command buffer failed: %s (before submit %u)", gpu_error, state.submits + 1);
    state.submits++; replay_queue = "main";
    unsigned before = state.commands, clears = state.clears;
    for (int i = 0; i < count; i++) {
        if (!wonder_nvn_command_handle_valid(handles[i]))
            wonder_nvn_stop("NVN_INVALID_HANDLE nvnQueueSubmitCommands handle[%d]=%#llx", i, (unsigned long long)handles[i]);
        replay_handle(handles[i], 0);
    }
    if (census_only) {
        char list[sizeof missing_list]; unsigned programs = missing_programs;
        memcpy(list, missing_list, sizeof list); census_only = 0; missing_programs = 0; missing_list[0] = 0;
        atomic_store(&replay_owner, (pthread_t *)NULL);
        wonder_nvn_stop("NVN_SHADER_MISSING %u programs (vs-fs): %s; census=DrawCensus/<vs>-<fs>, build them with tools/wonder_shader_build.py (submit %u)",
                        programs, list, state.submits);
    }
    if (first_draw_pending) { first_draw_pending = 0; first_draw_captured = 1; wonder_host_capture_first_draw(&first_draw_target); }
    // Diagnostic (WONDER_SUBMIT_SYNC=1): wait for the GPU after every submit, to tell CPU/GPU memory races from
    // wrong rendering.
    { static int sync = -1; if (sync < 0) { const char *e = getenv("WONDER_SUBMIT_SYNC"); sync = e && *e == '1'; }
      if (sync) wonder_host_finish(); }
    unsigned submits = state.submits, commands = state.commands - before; clears = state.clears - clears;
    atomic_store(&replay_owner, (pthread_t *)NULL);
    wonder_host_note_submit(submits);
    if (submits <= 8 || submits % 600 == 0 || submit_programs[0])
        wonder_nvn_report("NVN_QUEUE_SUBMIT queue=%p submit=%u handles=%d commands=%u clears=%u draws=%u programs=%s", queue, submits, count, commands, clears,
                          submit_draws, submit_programs[0] ? submit_programs : "-");
    submit_draws = 0; submit_programs[0] = 0;
}
void wonder_nvnQueueFlush(void *queue) { wonder_nvn_queue_check(queue, "nvnQueueFlush"); wonder_host_flush(); }
void wonder_nvnQueueFinish(void *queue) { wonder_nvn_queue_check(queue, "nvnQueueFinish"); wonder_host_finish(); }

const struct wonder_nvn_entry wonder_nvn_queue_entries[] = {
    {"nvnQueueSubmitCommands", (void *)wonder_nvnQueueSubmitCommands},
    {"nvnQueueFlush", (void *)wonder_nvnQueueFlush},
    {"nvnQueueFinish", (void *)wonder_nvnQueueFinish},
};
const unsigned wonder_nvn_queue_count = sizeof wonder_nvn_queue_entries / sizeof *wonder_nvn_queue_entries;
