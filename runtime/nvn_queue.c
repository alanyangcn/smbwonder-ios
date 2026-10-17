// NVN queue submission for Wonder: replays recorded command handles onto the host Metal queue.
// Executed here: render-target clears, buffer fills, uniform updates, timestamp/value reports, barriers and
// all state tracking (persistent per queue, as GPU state is). Not executed yet: draws, dispatches, texture
// copies/downsamples/clears, non-timestamp counters (TIMESTAMP and TIMESTAMP_TOP run), conditional rendering: replay stops by name before any
// of them is skipped or faked. Host backend: host_graphics.m (device) or the test harness.
#include "gpu_gate.h"
#include "access_watch.h"
#include "nvn_commands.h"
#include "nvn_tex.h"
#include "nvn_draw_census.h"
#include "report_shadow.h"
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
static _Atomic unsigned frame_draws, frame_index; static _Atomic uint64_t frame_hash = 1469598103934665603ull;
// Every present advances the frame number the per-frame diagnostics use (WONDER_DRAW_LOG, WONDER_FRAME_DUMP_EVERY);
// before round153 only WONDER_PRESENT_EVERY advanced it, so frame dumps never fired.
void wonder_nvn_frame_advance(void) { atomic_fetch_add(&frame_index, 1); }
unsigned wonder_nvn_frame_number(void) { return atomic_load(&frame_index); }
void wonder_nvn_frame_summary(unsigned *draws, unsigned *clears, uint64_t *hash) {
    static unsigned clears_at_last; unsigned c = state.clears;
    *draws = atomic_exchange(&frame_draws, 0); *hash = atomic_exchange(&frame_hash, 1469598103934665603ull);
    *clears = c - clears_at_last; clears_at_last = c;
}
// Bound program, vertex layout, buffers, textures and fixed-function state, kept for draws (and the draw census).
static struct wonder_nvn_draw_state draw_state;
// Pending GPU writes (report_shadow.h, declared above make_cpu_visible): conditional renders read their records without
// finishing the GPU when no unknown write touches them. WONDER_VERIFY_CONDITIONAL=1 finishes anyway and counts mismatches.
static unsigned conditional_shadowed, conditional_mismatches;
// UpdateUniformBuffer ranges recorded since the replay last finished the GPU (GPU-ordered writes the CPU cannot see yet,
// including earlier submits still pending). Reading such memory on the CPU first finishes the work recorded so far.
static struct { uint64_t address, size; } ubo_updates[64]; static unsigned ubo_update_count, ubo_update_overflow;
static struct wonder_report_shadow report_shadow;
static void replay_finish(void) {   // GPU done: every recorded write has landed
    wonder_host_finish(); ubo_update_count = 0; ubo_update_overflow = 0; wonder_report_shadow_clear(&report_shadow);
}
static void make_cpu_visible(uint64_t address, uint64_t size) {
    int overlap = ubo_update_overflow;
    for (unsigned i = 0; !overlap && i < ubo_update_count; i++) overlap = address < ubo_updates[i].address + ubo_updates[i].size && ubo_updates[i].address < address + size;
    if (!overlap) return;
    replay_finish();
}
// Rasterizer state the Metal path cannot honour except at its NVN defaults (refused at draw time when changed).
static struct { int rasterizer_discard, depth_clamp, restart, restart_index; uint32_t sample_mask; float line_width;
                float offset_factor, offset_units, offset_clamp; }
    raster = {.sample_mask = 0xffffffffu, .line_width = 1.0f};
// Census-only: after the first draw whose program has no translated shaders, the rest of the submit only tracks state and
// writes a census for every further missing program; no GPU work of this submit runs after that point, and the submit
// stops by name at its end (nothing is presented from a partly executed frame).
static int census_only; static unsigned missing_programs; static char missing_list[512];
// Diagnostic sweep (WONDER_CENSUS_SWEEP=1, for collecting censuses only, never a playable state): a draw whose program
// has no translated shaders writes its census and is not executed; everything else in the submit runs and the game
// continues, so one run collects every program it reaches. Frames with such draws are visibly incomplete.
static int census_sweep(void) { static int v = -1; if (v < 0) { const char *e = getenv("WONDER_CENSUS_SWEEP"); v = e && *e == '1'; } return v; }
static unsigned sweep_skipped;
static char missing_pairs[256][34];   // dedup independent of the (truncated) message list
// Per-submit draw summary for the log (which programs a failing command buffer contained).
static unsigned submit_draws; static char submit_programs[256];
// The first executed draw's colour target is read back (evidence) after the submit that holds it.
static struct wonder_nvn_texture_snapshot first_draw_target; static int first_draw_pending, first_draw_captured;
// Text targets captured after their submit (diagnostic, round132: text still invisible).
void wonder_host_capture_target(const struct wonder_nvn_texture_snapshot *target, const char *name);
int wonder_host_copy_texture(const struct wonder_nvn_texture_snapshot *s, const struct wonder_nvn_copy_region *sr,
                             const struct wonder_nvn_texture_snapshot *d, const struct wonder_nvn_copy_region *dr, int flags, char *why, size_t n);
static struct wonder_nvn_texture_snapshot text_targets[3]; static int text_pending;
// Diagnostic (WONDER_FRAME_DUMP_EVERY=<n>): in every n-th presented frame, each colour target a submit drew to is read back
// after that submit (Documents/FrameDump/f<frame>-s<submit>-<k>-t<serial>.png) and every draw is logged (NVN_DRAW_LOG),
// to find the pass that produces a wrong picture.
static struct wonder_nvn_texture_snapshot dump_targets[24]; static int dump_count;
// Diagnostic (WONDER_DRAW_DUMP=<n>): in every n-th frame, the colour target 0 of each run of draws with one program is
// read back when the program changes (Documents/DrawDump/f<frame>-<run>-<vs>-<fs>-t<serial>.png; 900 files at most),
// to see which draw produces a wrong region (round154b: flat blue foreground bushes).
static struct wonder_nvn_texture_snapshot run_target, run_depth; static char run_vs[17], run_fs[17]; static int run_valid, run_seq; static unsigned run_frame;
static int draw_dump_frame(void) { static int n = -1; if (n < 0) { const char *e = getenv("WONDER_DRAW_DUMP"); n = e ? atoi(e) : 0; if (n < 0) n = 0; } return n; }
static void draw_dump_flush(void) {
    if (!run_valid || run_seq >= 900) { run_valid = 0; return; }
    char name[120]; snprintf(name, sizeof name, "DrawDump/f%u-%03d-%.8s-%.8s-t%llu.png", run_frame, run_seq++, run_vs, run_fs, (unsigned long long)run_target.texture.serial);
    wonder_host_capture_target(&run_target, name);
    if (run_depth.valid) {   // the depth attachment too, as grey (round168: which pass leaves a near depth over the ground)
        snprintf(name, sizeof name, "DrawDump/f%u-%03d-%.8s-%.8s-z%llu.png", run_frame, run_seq - 1, run_vs, run_fs, (unsigned long long)run_depth.texture.serial);
        wonder_host_capture_target(&run_depth, name);
    }
    run_valid = 0;
}
// Draws left out by NVN render enable (SetRenderEnable(false) or a conditional render that evaluated to skip), reported
// with NVN_QUEUE_SUBMIT every 600 submits (round171: are course hedges among them?).
static unsigned disabled_draws, conditional_evals, conditional_skips;
static int trace_replay_vs;   // the bound program's vertex shader is in WONDER_TRACE_VS
static int frame_dump_every(void) { static int n = -1; if (n < 0) { const char *e = getenv("WONDER_FRAME_DUMP_EVERY"); n = e ? atoi(e) : 0; if (n < 0) n = 0; } return n; }
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
    {   // Diagnostic (WONDER_WATCH_RT=<W>x<H>[,...]): the pool memory of render targets of those sizes is access-watched
        // (access_watch.h), once per storage: does the CPU read what the GPU renders there? (round180)
        const char *list = getenv("WONDER_WATCH_RT");
        if (list && *list) {
            char size[24]; snprintf(size, sizeof size, "%ux%u", t->desc.width, t->desc.height);
            int listed = 0; for (const char *q = list; *q && !listed; ) { size_t n = strcspn(q, ","); listed = n == strlen(size) && !strncmp(q, size, n); q += n; if (*q == ',') q++; }
            static struct { const void *pool; int64_t offset; } armed[128]; static unsigned narmed;
            int known = 0; for (unsigned i = 0; i < narmed && !known; i++) known = armed[i].pool == t->pool && armed[i].offset == t->offset;
            if (listed && !known && narmed < 128) {
                struct wonder_nvn_pool_view v; wonder_nvn_pool_span(t->pool, (uint64_t)t->offset, t->size, &v);
                char label[48]; snprintf(label, sizeof label, "rt %s fmt %#x serial %llu", size, t->desc.format, (unsigned long long)t->serial);
                if (v.memory && wonder_access_watch(v.memory, t->size, label)) {
                    armed[narmed].pool = t->pool; armed[narmed].offset = t->offset; narmed++;
                    wonder_nvn_report("NVN_WATCH_RT armed %s at %p size %#llx", label, v.memory, (unsigned long long)t->size);
                }
            }
        }
    }
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

// WONDER_DRAW_LOG=<first>-<last> present numbers: every draw (and colour clear) of those frames is logged. The bounds
// are plain statics so a debugger can open a window on a running game (round189).
static int draw_log_first = -1, draw_log_last = -1;
static int draw_log_frame(unsigned f) {
    if (draw_log_first < 0) { const char *e = getenv("WONDER_DRAW_LOG"); draw_log_first = 0; draw_log_last = -1; if (e) sscanf(e, "%d-%d", &draw_log_first, &draw_log_last); }
    return (int)f >= draw_log_first && (int)f <= draw_log_last;
}
struct report { uint64_t address; uint32_t value; int timestamp; };
static void write_report(void *arg) {
    struct report *r = arg; struct wonder_nvn_pool_view view;
    wonder_nvn_address_span(r->address, r->timestamp ? 16 : 4, &view);
    if (r->timestamp) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t data[2] = {0, (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec};   // NVNcounterData {counter, timestamp ns}
        {   // Diagnostic (WONDER_DIAG_FREEZE_TS=1): every GPU timestamp reads the same value, so measured GPU time is 0
            // (round185: does the game's dynamic resolution / quality drop the course hedges' colour passes?)
            static int on = -1; if (on < 0) { const char *e = getenv("WONDER_DIAG_FREEZE_TS"); on = e && *e == '1'; }
            static uint64_t frozen; if (on) { if (!frozen) frozen = data[1]; data[1] = frozen; }
        }
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
// Texture slot (or bindless handle) of NVN stage `stage` read by reflection entry rt -> out->tex[out_stage][slot]: the pool
// registration, its sampler and, for textures without a host GPU copy, the pool storage they are uploaded from.
static void resolve_texture(unsigned opcode, int stage, const struct wonder_shader_tex *rt, struct wonder_host_draw *out, int out_stage) {
    char why[320]; int slot = rt->slot; uint64_t h;
    if (rt->bindless) {   // combined handle stored by the game in a uniform buffer (round86)
        uint64_t base = draw_state.uniform_buffer[stage][rt->ubo_slot].address, size = draw_state.uniform_buffer[stage][rt->ubo_slot].size;
        if (!base || rt->offset + 4u > size) { snprintf(why, sizeof why, "bindless texture handle: uniform slot %d not bound or too small (offset %#x)", rt->ubo_slot, rt->offset); replay_stop(opcode, why); }
        make_cpu_visible(base + rt->offset, 4);   // round87: the handle was written by UpdateUniformBuffer in this submit
        struct wonder_nvn_pool_view v; wonder_nvn_address_span(base + rt->offset, 4, &v); uint32_t raw; memcpy(&raw, v.memory, 4); h = raw;
    } else h = draw_state.texture_handle[stage][slot];
    // Handle 0 names texture and sampler descriptor 0, inside the 256 descriptors NVN reserves for the driver: the game's
    // "nothing bound" (round151: a fragment shader samples slot 12 after loading a save; D2R nvn_mtl.m binds an all-zero
    // texture for it too). Sampling it reads zeros; any other unresolvable handle still stops.
    if (!h) {
        static unsigned logged; if (logged++ < 16) wonder_nvn_report("NVN_EMPTY_TEXTURE_SLOT stage %d slot %d type %d vs=%s fs=%s (handle 0, reads 0)", stage, slot, rt->type, out->vs_key, out->fs_key);
        memset(&out->tex[out_stage][slot], 0, sizeof out->tex[out_stage][slot]); out->tex[out_stage][slot].empty = 1; out->tex_mask[out_stage] |= 1ull << slot;
        return;
    }
    const struct wonder_nvn_texture_registration *t = h && draw_state.texture_pool ? wonder_nvn_texture_lookup(draw_state.texture_pool, (int)(h & 0xfffff)) : NULL;
    const struct wonder_nvn_sampler_state *m = h && draw_state.sampler_pool ? wonder_nvn_sampler_lookup(draw_state.sampler_pool, (int)((h >> 20) & 0xfff)) : NULL;
    if (!t || !t->valid || !m || !m->valid) { snprintf(why, sizeof why, "unbound texture stage %d slot %d (handle %#llx)", stage, slot, (unsigned long long)h); replay_stop(opcode, why); }
    // NVN target (1D 0, 2D 1, 3D 2, 1D_ARRAY 3, 2D_ARRAY 4, 2DMS 5, 2DMS_ARRAY 6, RECTANGLE 7, CUBE 8, CUBE_ARRAY 9, BUFFER 10) -> reflection type.
    static const int target_type[11] = {WONDER_SHADER_TEX_1D, WONDER_SHADER_TEX_2D, WONDER_SHADER_TEX_3D, WONDER_SHADER_TEX_1D_ARRAY, WONDER_SHADER_TEX_2D_ARRAY,
        WONDER_SHADER_TEX_2D_MS, WONDER_SHADER_TEX_2D_MS_ARRAY, WONDER_SHADER_TEX_2D, WONDER_SHADER_TEX_CUBE, WONDER_SHADER_TEX_CUBE_ARRAY, WONDER_SHADER_TEX_BUFFER};
    int target = t->has_view && t->view.set_target ? t->view.target : t->texture.desc.target;
    // The sampling instruction decides the dimension on Maxwell: a 2D texture sampled as a 1-layer array, and a 2D array
    // sampled by a 2D instruction (round153) reads its first layer (metal3_draw.m binds a 2D view of it).
    int as_layer = (target == 1 || target == 7) && rt->type == WONDER_SHADER_TEX_2D_ARRAY, first_layer = target == 4 && rt->type == WONDER_SHADER_TEX_2D;
    {   // Diagnostic (WONDER_DIAG_CUBE_AS_EMPTY=1): a cube declaration over a bound 2D texture samples zeros instead of
        // stopping (round186: a quality path the game takes with frozen GPU timestamps); not a product behaviour.
        static int on = -1; if (on < 0) { const char *e = getenv("WONDER_DIAG_CUBE_AS_EMPTY"); on = e && *e == '1'; }
        if (on && (target == 1 || target == 7) && (rt->type == WONDER_SHADER_TEX_CUBE || rt->type == WONDER_SHADER_TEX_CUBE_ARRAY)) {
            static unsigned logged; if (logged++ < 8) wonder_nvn_report("NVN_DIAG_CUBE_AS_EMPTY stage %d slot %d (vs=%s fs=%s)", stage, slot, out->vs_key, out->fs_key);
            memset(&out->tex[out_stage][slot], 0, sizeof out->tex[out_stage][slot]); out->tex[out_stage][slot].empty = 1; out->tex_mask[out_stage] |= 1ull << slot;
            return;
        }
    }
    if (target < 0 || target > 10 || (target_type[target] != rt->type && !as_layer && !first_layer)) {
        snprintf(why, sizeof why, "texture type: stage %d slot %d bound target %d, shader declares type %d (vs=%s fs=%s)", stage, slot, target, rt->type, out->vs_key, out->fs_key); replay_stop(opcode, why);
    }
    out->tex[out_stage][slot].texture = *t; out->tex[out_stage][slot].sampler = *m; out->tex_mask[out_stage] |= 1ull << slot;
    if (!t->texture.host) {   // display textures are GPU-only; others are uploaded from their pool storage
        struct wonder_nvn_pool_view v; wonder_nvn_pool_span(t->texture.pool, (uint64_t)t->texture.offset, t->texture.size, &v);
        if (v.size < t->texture.size || !v.memory) { snprintf(why, sizeof why, "texture stage %d slot %d storage is not CPU-resolvable", stage, slot); replay_stop(opcode, why); }
        out->tex[out_stage][slot].storage = v.memory; out->tex[out_stage][slot].storage_size = t->texture.size;
        out->tex[out_stage][slot].storage_buffer = v.host_buffer; out->tex[out_stage][slot].storage_buffer_offset = v.host_offset;
    }
}
// Driver constant segment c[0] as the NVN driver lays it out for the translated shaders' address arithmetic: per stage
// its storage table (vertex 0x110, compute 0x310, fragment 0x510; 16 bytes per slot: u64 GPU address, u32 size) and the
// compute uniform address table at 0x210 (wonder_shader_build.py check_driver_c0 limits reads to these tables).
static void driver_c0(uint8_t out[0x700], int compute) {
    memset(out, 0, 0x700);
    static const struct { int stage, base; } storage[] = {{0, 0x110}, {5, 0x310}, {1, 0x510}};
    for (unsigned t = 0; t < 3; t++) {
        if ((storage[t].stage == 5) != compute) continue;
        for (int r = 0; r < 16; r++) {
            uint64_t a = draw_state.storage_buffer[storage[t].stage][r].address; uint32_t n = (uint32_t)draw_state.storage_buffer[storage[t].stage][r].size;
            memcpy(out + storage[t].base + 16 * r, &a, 8); memcpy(out + storage[t].base + 16 * r + 8, &n, 4);
        }
    }
    if (compute) for (int r = 0; r < 14; r++) {
        uint64_t a = draw_state.uniform_buffer[5][r].address; uint32_t n = (uint32_t)draw_state.uniform_buffer[5][r].size;
        memcpy(out + 0x210 + 16 * r, &a, 8); memcpy(out + 0x210 + 16 * r + 8, &n, 4);
    }
}
// A program without translated shaders: noted once in the missing list (census written by the caller).
static void note_missing(const char *pair);
// Census sweep (round193): an offline-built stage whose assumed float attribute / output type differs from what the draw
// binds is recorded like a missing program (census written, this draw left out); the rebuild takes the census's bound
// types, which win over the offline guess. Outside the sweep the caller stops by name.
static int sweep_type_census(const struct wonder_nvn_draw_call *call, const char *vs, const char *fs, const char *why, char *summary, size_t size) {
    if (!census_sweep()) return 0;
    wonder_nvn_draw_census(&draw_state, call, summary, size);
    char pair[40]; snprintf(pair, sizeof pair, "%s-%s", vs, fs); note_missing(pair);
    static _Atomic unsigned logged; if (atomic_fetch_add(&logged, 1) < 64) wonder_nvn_report("NVN_CENSUS_SWEEP_TYPE %s (%s)", why, summary);
    sweep_skipped++; return 1;
}
static void note_missing(const char *pair) {
    for (unsigned i = 0; i < missing_programs && i < 256; i++) if (!strcmp(missing_pairs[i], pair)) return;
    if (missing_programs < 256) snprintf(missing_pairs[missing_programs], sizeof missing_pairs[0], "%s", pair);
    size_t used = strlen(missing_list);
    if (used + strlen(pair) + 2 < sizeof missing_list) snprintf(missing_list + used, sizeof missing_list - used, "%s%s", used ? " " : "", pair);
    missing_programs++;
}
static void draw(unsigned opcode, struct wonder_nvn_draw_call call) {
    call.name = wonder_nvn_command_name(opcode);
    sync_draw_state();
    char why[320], summary[160];
    if ((draw_state.program.mask & 0x1f) != 3) {   // graphics stages: vertex + fragment only (a bound compute shader is not part of draws)
        wonder_nvn_draw_census(&draw_state, &call, summary, sizeof summary);
        snprintf(why, sizeof why, "draw needs a vertex+fragment program (stages %#x); %s", draw_state.program.mask, summary);
        replay_stop(opcode, why);
    }
    if (raster.rasterizer_discard) replay_stop(opcode, "rasterizer discard is enabled (not executed yet)");
    if (raster.sample_mask != 0xffffffffu) { snprintf(why, sizeof why, "sample mask %#x is not all samples", raster.sample_mask); replay_stop(opcode, why); }
    if ((call.primitive == 1 || call.primitive == 2 || call.primitive == 3) && raster.line_width != 1.0f) replay_stop(opcode, "line width other than 1 (Metal draws 1-pixel lines)");
    // Polygon offset: the enable bit of the polygon mode (POINT 1, LINE 2, FILL 4 for modes 0..2) applies to triangles,
    // with SetPolygonOffsetClamp's GL-style factor (slope) / units (constant) / clamp, which Metal takes as is.
    int bias = (draw_state.states_set & 8) && (draw_state.polygon.offset_en >> draw_state.polygon.mode & 1) &&
               ((call.primitive >= 4 && call.primitive <= 9) || call.primitive == 12 || call.primitive == 13);   // triangle kinds
    int depth_mode, origin; wonder_nvn_device_modes(&depth_mode, &origin);
    if (origin != 1) { snprintf(why, sizeof why, "window origin %d (only UPPER_LEFT matches the translated shaders)", origin); replay_stop(opcode, why); }
    static struct wonder_host_draw d; memset(&d, 0, sizeof d);
    memcpy(d.vs_key, draw_state.program.key[0], 17); memcpy(d.fs_key, draw_state.program.key[1], 17);
    if (!wonder_host_shader_available(d.vs_key) || !wonder_host_shader_available(d.fs_key)) {
        wonder_nvn_draw_census(&draw_state, &call, summary, sizeof summary);
        char pair[40]; snprintf(pair, sizeof pair, "%s-%s", d.vs_key, d.fs_key); note_missing(pair);
        if (census_sweep()) { sweep_skipped++; return; }   // only this draw is left out
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
    {   // Offline-built vertex stages read every attribute as float; integer formats bound there get the stage rewritten the
        // way the translator writes integer attributes (round196: vs 01b4c315 attribute 6 R16UI). A stage that declares an
        // integer attribute but is bound a float format still stops by name below.
        uint8_t retype[32] = {0}; int any = 0;
        for (int i = 0; i < d.vr->nattr; i++) {
            int a = d.vr->attr[i].nvn;
            if (a >= draw_state.nattrib || !draw_state.attrib[a].fmt || d.vr->attr[i].type != WONDER_SHADER_FLOAT) continue;
            int cls = wonder_shader_format_class((int)draw_state.attrib[a].fmt);
            if (cls != WONDER_SHADER_FLOAT && d.vr->attr[i].location < 32) { retype[d.vr->attr[i].location] = (uint8_t)cls; any = 1; }
        }
        d.vs = any ? wonder_host_shader_function_retyped(d.vs_key, retype, &d.vr) : wonder_host_shader_function(d.vs_key, 0);
    }
    d.fs = wonder_host_shader_function(d.fs_key, 1);
    if (d.vr->drv0_buffer >= 0 || d.fr->drv0_buffer >= 0) driver_c0(d.drv0, 0);
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
            if (r->sbuf[i].write) wonder_report_shadow_unknown(&report_shadow, draw_state.storage_buffer[stage][slot].address, draw_state.storage_buffer[stage][slot].size);
        }
        for (int i = 0; i < r->ntex; i++) resolve_texture(opcode, stage, &r->tex[i], &d, stage);
    }
    for (int i = 0; i < d.vr->nattr; i++) {
        int a = d.vr->attr[i].nvn;
        if (a >= draw_state.nattrib || !draw_state.attrib[a].fmt) {   // disabled: Maxwell reads (0,0,0,1) (round92)
            d.attr[i].format = 0; d.attr[i].offset = 0; d.attr[i].stream = WONDER_HOST_DEFAULT_ATTRIBUTE; continue;
        }
        if (wonder_shader_format_class((int)draw_state.attrib[a].fmt) != d.vr->attr[i].type) {   // offline-built stages assume float (round153)
            snprintf(why, sizeof why, "vertex attribute %d format %#x is %s, the vertex shader %s reads it as %s", a, draw_state.attrib[a].fmt,
                     (const char *[]){"float", "uint", "sint"}[wonder_shader_format_class((int)draw_state.attrib[a].fmt)], d.vs_key, (const char *[]){"float", "uint", "sint"}[d.vr->attr[i].type % 3]);
            if (sweep_type_census(&call, d.vs_key, d.fs_key, why, summary, sizeof summary)) return;
            replay_stop(opcode, why);
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
    d.frame = atomic_load(&frame_index); d.depth_clamp = raster.depth_clamp; d.depth_bias_enabled = bias; d.depth_bias = raster.offset_units; d.depth_bias_slope = raster.offset_factor; d.depth_bias_clamp = raster.offset_clamp;
    if (call.indirect_address) replay_stop(opcode, "indirect draws are not executed yet");
    if (call.index_type >= 0) {
        d.index = span_of(opcode, call.index_address, (uint64_t)call.count << call.index_type);
        // Metal restarts strips on the all-ones index and never restarts lists; NVN restarts on its own index when enabled.
        uint32_t all_ones = call.index_type == 0 ? 0xffu : call.index_type == 1 ? 0xffffu : 0xffffffffu;
        int strip = call.primitive == 3 || call.primitive == 5;
        if (strip && !(raster.restart && (uint32_t)raster.restart_index == all_ones)) {
            // Metal restarts strips at the all-ones index regardless; without NVN restart (or with another restart
            // index) the draw matches only when that value never occurs (round147: a strip draw with restart off).
            struct wonder_nvn_pool_view v; wonder_nvn_address_span(call.index_address, (uint64_t)call.count << call.index_type, &v);
            for (int i = 0; i < call.count; i++) {
                uint32_t x = call.index_type == 0 ? v.memory[i] : call.index_type == 1 ? ((const uint16_t *)v.memory)[i] : ((const uint32_t *)v.memory)[i];
                if (x == all_ones) replay_stop(opcode, "strip draw uses the all-ones index without NVN primitive restart on it (Metal would restart there)");
                if (raster.restart && x == (uint32_t)raster.restart_index) replay_stop(opcode, "strip draw restarts on an index other than all-ones (Metal restarts only there)");
            }
        }
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
    for (int i = 0; i < d.color_count && i < 8; i++)   // colour outputs: a float output into an integer target (or back) is not a conversion
        if (d.targets[i].valid && (d.fr->fout_mask >> i & 1) && wonder_shader_format_class(d.targets[i].texture.desc.format) != d.fr->fout_type[i]) {
            snprintf(why, sizeof why, "colour target %d format %#x is %s, the fragment shader %s writes %s", i, d.targets[i].texture.desc.format,
                     (const char *[]){"float", "uint", "sint"}[wonder_shader_format_class(d.targets[i].texture.desc.format)], d.fs_key, (const char *[]){"float", "uint", "sint"}[d.fr->fout_type[i] % 3]);
            if (sweep_type_census(&call, d.vs_key, d.fs_key, why, summary, sizeof summary)) return;
            replay_stop(opcode, why);
        }
    memcpy(d.blend, draw_state.blend, sizeof d.blend); d.blend_set = draw_state.blend_set; memcpy(d.blend_color, state.blend_color, sizeof d.blend_color);
    d.color = draw_state.color; d.channel_mask = draw_state.channel_mask; d.depth_stencil = draw_state.depth_stencil;
    d.polygon = draw_state.polygon; d.multisample = draw_state.multisample; d.states_set = draw_state.states_set;
    memcpy(d.viewport, state.viewport, sizeof d.viewport); d.viewport_set = state.viewport_set;
    memcpy(d.depth_range, state.depth_range, sizeof d.depth_range);
    memcpy(d.scissor, state.scissor, sizeof d.scissor); d.scissor_set = state.scissor_set;
    d.depth_mode = depth_mode; d.window_origin = origin;
    // Diagnostic (round128: text never visible): the font shaders' instanced draws, their per-glyph storage records
    // (64 bytes each: transform, uv rect, colour, layer at +40) and the c3 transform rows.
    if (!strcmp(d.fs_key, "929a2e26d5411dce") || !strcmp(d.fs_key, "60ac11859809685a")) {
        static _Atomic int logged; int n = atomic_fetch_add(&logged, 1);
        if (n < 6) {
            uint64_t sa = draw_state.storage_buffer[0][0].address, ss = draw_state.storage_buffer[0][0].size;
            uint64_t ua = draw_state.uniform_buffer[0][0].address, us = draw_state.uniform_buffer[0][0].size;
            char line[1600]; int k = snprintf(line, sizeof line, "HOST_TEXT_DRAW #%d %s-%s prim=%d count=%d first=%d instances=%d base_instance=%d sbuf=%#llx+%#llx c3=%#llx+%#llx",
                n, d.vs_key, d.fs_key, call.primitive, call.count, call.first, call.instances, call.base_instance,
                (unsigned long long)sa, (unsigned long long)ss, (unsigned long long)ua, (unsigned long long)us);
            if (sa && ss >= 64) { make_cpu_visible(sa, 64); struct wonder_nvn_pool_view v; wonder_nvn_address_span(sa, 64, &v); const float *f = (const float *)v.memory; const int32_t *iv = (const int32_t *)v.memory;
                k += snprintf(line + k, sizeof line - k, " inst0=[%g %g %g %g | %g %g %g %g | %#x %#x layer=%d %d | %g %g %g %g]", f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], iv[8], iv[9], iv[10], iv[11], f[12], f[13], f[14], f[15]);
                extern uint64_t wonder_nvn_glyph_cache_rect_sum(float, float, float, float, unsigned *); unsigned px = 0;
                uint64_t sum = wonder_nvn_glyph_cache_rect_sum(f[4], f[5], f[6], f[7], &px);
                k += snprintf(line + k, sizeof line - k, " glyph_rect_sum=%llu/%u px", (unsigned long long)sum, px); }
            if (ua && us >= 64) { make_cpu_visible(ua, 64); struct wonder_nvn_pool_view v; wonder_nvn_address_span(ua, 64, &v); const float *f = (const float *)v.memory;
                k += snprintf(line + k, sizeof line - k, " c3=[%g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g]", f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]); }
            uint64_t fa = draw_state.uniform_buffer[1][0].address, fs = draw_state.uniform_buffer[1][0].size;
            if (fa && fs >= 128) { make_cpu_visible(fa, 128); struct wonder_nvn_pool_view v; wonder_nvn_address_span(fa, 128, &v); const float *f = (const float *)v.memory;
                k += snprintf(line + k, sizeof line - k, " fs_c3[4..7]=[%g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g]", f[16], f[17], f[18], f[19], f[20], f[21], f[22], f[23], f[24], f[25], f[26], f[27], f[28], f[29], f[30], f[31]); }
            uint64_t fb = draw_state.uniform_buffer[1][1].address;
            if (fb) { make_cpu_visible(fb, 4); struct wonder_nvn_pool_view v; wonder_nvn_address_span(fb, 4, &v); k += snprintf(line + k, sizeof line - k, " fs_c4.x=%#x", *(const uint32_t *)v.memory); }
            { const struct wonder_nvn_sampler_state *m = &d.tex[1][0].sampler; const struct wonder_nvn_texture_registration *r = &d.tex[1][0].texture;
              wonder_nvn_report("HOST_TEXT_SAMPLER #%d smp=min%u mag%u wrap%u/%u/%u cmp%u/%u lod%g..%g bias%g aniso%g red%u tex=%llu view=%d lv=%d/%d", n,
                            m->min, m->mag, m->wrap_s, m->wrap_t, m->wrap_r, m->compare_mode, m->compare_func, m->lod_min, m->lod_max, m->lod_bias, m->anisotropy,
                            m->reduction, (unsigned long long)r->texture.serial, r->has_view, r->view.set_levels ? r->view.base_level : -1, r->view.set_levels ? r->view.levels : -1); }
            k += snprintf(line + k, sizeof line - k, " blend_en=%#x blend0=%u/%u/%u %u/%u/%u mask=%#x depth=%u/%u/%u", d.color.blend_en, d.blend[0].src_rgb, d.blend[0].dst_rgb, d.blend[0].eq_rgb,
                          d.blend[0].src_a, d.blend[0].dst_a, d.blend[0].eq_a, d.channel_mask.mask, d.depth_stencil.dt, d.depth_stencil.dw, d.depth_stencil.df);
            if (n < 3 && d.targets[0].valid && text_pending < 3) text_targets[text_pending++] = d.targets[0];
            k += snprintf(line + k, sizeof line - k, " vp=%s%g,%g %gx%g sc=%s%d,%d %dx%d dr=%g..%g", d.viewport_set ? "" : "(unset)", d.viewport[0], d.viewport[1], d.viewport[2], d.viewport[3],
                          d.scissor_set ? "" : "(unset)", d.scissor[0], d.scissor[1], d.scissor[2], d.scissor[3], d.depth_range[0], d.depth_range[1]);
            for (int i = 0; i < d.color_count && i < 1; i++) if (d.targets[i].valid)
                k += snprintf(line + k, sizeof line - k, " rt0=%llu:%ux%u fmt=%#x", (unsigned long long)d.targets[i].texture.serial, d.targets[i].texture.desc.width, d.targets[i].texture.desc.height, d.targets[i].texture.desc.format);
            wonder_nvn_report("%s", line);
        }
    }
    if (wonder_host_draw(&d)) replay_stop(opcode, "host draw encoding failed");
    if (!first_draw_captured && !first_draw_pending)
        for (int i = 0; i < d.color_count; i++) if (d.targets[i].valid) { first_draw_target = d.targets[i]; first_draw_pending = 1; break; }
    submit_draws++;
    { // Diagnostic (WONDER_DRAW_LOG=<first>-<last> present numbers, with WONDER_PRESENT_EVERY): every draw's targets and
      // textures by storage, to compare frames that execute the same draw sequence.
      unsigned f = atomic_load(&frame_index) + 1; int dump = frame_dump_every() && f % (unsigned)frame_dump_every() == 0;
      if (dump) for (int i = 0; i < 9; i++) if (d.targets[i].valid) {
          int known = 0; const struct wonder_nvn_texture_snapshot *t = &d.targets[i];
          for (int k = 0; k < dump_count && !known; k++) known = dump_targets[k].texture.serial == t->texture.serial && dump_targets[k].has_view == t->has_view &&
              (!t->has_view || (dump_targets[k].view.base_level == t->view.base_level && dump_targets[k].view.min_layer == t->view.min_layer));
          if (!known && dump_count < (int)(sizeof dump_targets / sizeof *dump_targets)) dump_targets[dump_count++] = *t;
      }
      if (draw_dump_frame() && f % (unsigned)draw_dump_frame() == 0) {
          run_frame = f;
          if (run_valid && (strcmp(run_vs, d.vs_key) || strcmp(run_fs, d.fs_key) || run_target.texture.serial != d.targets[0].texture.serial)) draw_dump_flush();
          if (d.color_count && d.targets[0].valid) { run_target = d.targets[0]; run_depth = d.color_count < 9 ? d.targets[d.color_count] : (struct wonder_nvn_texture_snapshot){0}; snprintf(run_vs, sizeof run_vs, "%s", d.vs_key); snprintf(run_fs, sizeof run_fs, "%s", d.fs_key); run_valid = 1; }
      }
      if (dump || draw_log_frame(f)) {
          char line[1024]; int n = snprintf(line, sizeof line, "NVN_DRAW_LOG present=%u %.8s-%.8s prim=%d count=%d inst=%d mask=%#x blend=%#x depth=%u/%u/%u cull=%u front=%u", f, d.vs_key, d.fs_key, d.primitive,
                                           call.count, call.instances, (d.states_set & 2) ? d.channel_mask.mask : 0xffffffffu, d.color.blend_en, d.depth_stencil.dt, d.depth_stencil.dw, d.depth_stencil.df, d.polygon.cull, d.polygon.front);
          for (int i = 0; i < 9 && n < (int)sizeof line - 80; i++) if (d.targets[i].valid) {
              const struct wonder_nvn_texture *t = &d.targets[i].texture;
              n += snprintf(line + n, sizeof line - n, " rt%d=%llu:%ux%u@%p+%#llx:%#x", i, (unsigned long long)t->serial, t->desc.width, t->desc.height, t->pool, (unsigned long long)t->offset, t->desc.format);
          }
          for (int st = 0; st < 2; st++) for (int i = 0; i < 40 && n < (int)sizeof line - 80; i++) if (d.tex_mask[st] >> i & 1) {
              const struct wonder_nvn_texture *t = &d.tex[st][i].texture.texture;
              n += snprintf(line + n, sizeof line - n, " t%d.%d=%llu:%ux%u@%p+%#llx:%#x", st, i, (unsigned long long)t->serial, t->desc.width, t->desc.height, t->pool, (unsigned long long)t->offset, t->desc.format);
          }
          wonder_nvn_report("%s", line);
      }
    }
    { uint64_t h = atomic_load(&frame_hash); for (const char *k = d.vs_key; *k; k++) h = (h ^ (uint8_t)*k) * 1099511628211ull;
      for (const char *k = d.fs_key; *k; k++) h = (h ^ (uint8_t)*k) * 1099511628211ull;
      h = (h ^ (uint32_t)call.count) * 1099511628211ull; atomic_store(&frame_hash, h); atomic_fetch_add(&frame_draws, 1); }
    { char pair[24]; snprintf(pair, sizeof pair, "%.8s-%.8s", d.vs_key, d.fs_key); size_t used = strlen(submit_programs);
      if (!strstr(submit_programs, pair) && used + strlen(pair) + 2 < sizeof submit_programs) snprintf(submit_programs + used, sizeof submit_programs - used, "%s%s", used ? "," : "", pair); }
    static _Atomic unsigned logged;
    if (atomic_fetch_add(&logged, 1) < 16)
        wonder_nvn_report("NVN_DRAW_EXECUTED %s vs=%s fs=%s count=%d instances=%d (submit %u command %u)", call.name, d.vs_key, d.fs_key, call.count, call.instances, state.submits, state.commands);
}
// Compute dispatches: the bound compute stage (BindProgram with the compute bit) resolved to its translated kernel and
// every buffer / texture / image its reflection reads, like a draw. A kernel without a translation writes its census.
static unsigned submit_dispatches;
static void dispatch(unsigned opcode, const struct wonder_nvn_cmd *c) {
    struct wonder_nvn_draw_call call = {.name = wonder_nvn_command_name(opcode), .compute = 1, .index_type = -1};
    if (opcode == WONDER_NVN_CMD_DISPATCH) { call.groups[0] = (int)c->a[0]; call.groups[1] = (int)c->a[1]; call.groups[2] = (int)c->a[2]; }
    else call.indirect_address = c->a[0];
    char why[320], summary[160];
    if (!(draw_state.program.mask & 0x20)) {
        snprintf(why, sizeof why, "dispatch without a bound compute shader (stages %#x)", draw_state.program.mask); replay_stop(opcode, why);
    }
    static struct wonder_host_dispatch x; memset(&x, 0, sizeof x);
    memcpy(x.key, draw_state.program.key[5], 17);
    {   // Diagnostic (WONDER_SKIP_CS=<kernel key prefix>[,...]): leave the listed dispatches out (round177: does kernel
        // 45bc61e5, run since the offline build, make the game stop issuing colour passes?)
        const char *list = getenv("WONDER_SKIP_CS");
        for (const char *q = list; q && *q; ) { size_t n = strcspn(q, ","); if (n && (!strncmp(q, x.key, n) || (n == 1 && *q == '*'))) {
            static _Atomic unsigned logged; if (atomic_fetch_add(&logged, 1) < 4) wonder_nvn_report("NVN_SKIP_CS %s (diagnostic)", x.key);
            return; } q += n; if (*q == ',') q++; }
    }
    if (!wonder_host_shader_available(x.key)) {
        wonder_nvn_draw_census(&draw_state, &call, summary, sizeof summary);
        char pair[40]; snprintf(pair, sizeof pair, "cs-%s", x.key); note_missing(pair);
        if (census_sweep()) { sweep_skipped++; return; }   // only this dispatch is left out
        if (!census_only) wonder_nvn_report("NVN_CENSUS_ONLY from command %u of submit %u: %s", state.commands, state.submits, summary);
        census_only = 1;
        return;
    }
    if (census_only) return;
    const struct wonder_shader_refl *r = x.r = wonder_host_shader_refl(x.key);
    if (r->stage != 5) replay_stop(opcode, "shader reflection stage does not match the compute stage");
    uint32_t local[3]; memcpy(local, (const uint8_t *)draw_state.program.control[5] + WONDER_NVN_CONTROL_LOCAL_SIZE, 12);
    if (memcmp(local, r->local, 12)) {
        snprintf(why, sizeof why, "kernel %s built for threadgroup %ux%ux%u, control says %ux%ux%u", x.key, r->local[0], r->local[1], r->local[2], local[0], local[1], local[2]);
        replay_stop(opcode, why);
    }
    if (r->drv_read) { snprintf(why, sizeof why, "compute reads a driver constant segment (c0/c2 mask %#x) at run time", r->drv_read); replay_stop(opcode, why); }
    x.cs = wonder_host_shader_function(x.key, 5);
    if (r->drv0_buffer >= 0) driver_c0(x.res.drv0, 1);
    for (int i = 0; i < r->ncbuf; i++) {
        int slot = r->cbuf[i].slot;
        if (!draw_state.uniform_buffer[5][slot].address) { snprintf(why, sizeof why, "unbound uniform buffer compute slot %d", slot); replay_stop(opcode, why); }
        x.ubo[slot] = span_of(opcode, draw_state.uniform_buffer[5][slot].address, draw_state.uniform_buffer[5][slot].size);
    }
    for (int i = 0; i < r->nuptr; i++) {
        int slot = r->uptr[i].slot;
        if (!draw_state.uniform_buffer[5][slot].address) { snprintf(why, sizeof why, "unbound uniform buffer compute slot %d (read by address)", slot); replay_stop(opcode, why); }
        x.uptr[slot] = span_of(opcode, draw_state.uniform_buffer[5][slot].address, draw_state.uniform_buffer[5][slot].size);
    }
    for (int i = 0; i < r->nsbuf; i++) {
        int slot = r->sbuf[i].slot;
        if (!draw_state.storage_buffer[5][slot].address || !draw_state.storage_buffer[5][slot].size) {
            snprintf(why, sizeof why, "unbound storage buffer compute slot %d", slot); replay_stop(opcode, why); }
        x.sbuf[slot] = span_of(opcode, draw_state.storage_buffer[5][slot].address, draw_state.storage_buffer[5][slot].size);
        if (r->sbuf[i].write) wonder_report_shadow_unknown(&report_shadow, draw_state.storage_buffer[5][slot].address, draw_state.storage_buffer[5][slot].size);
    }
    memcpy(x.res.vs_key, x.key, sizeof x.key); memcpy(x.res.fs_key, "-", 2);
    for (int i = 0; i < r->ntex; i++) resolve_texture(opcode, 5, &r->tex[i], &x.res, 0);
    for (int i = 0; i < r->nimg; i++) {
        int slot = r->img[i].slot; uint64_t h = draw_state.image_handle[5][slot];
        const struct wonder_nvn_texture_registration *t = h && h < (1u << 20) && draw_state.texture_pool ? wonder_nvn_texture_lookup(draw_state.texture_pool, (int)h) : NULL;
        if (!t || !t->valid) { snprintf(why, sizeof why, "unbound image compute slot %d (handle %#llx)", slot, (unsigned long long)h); replay_stop(opcode, why); }
        if (!(t->texture.desc.flags & 0x4)) { snprintf(why, sizeof why, "image slot %d texture serial %llu lacks NVN_TEXTURE_FLAGS_IMAGE", slot, (unsigned long long)t->texture.serial); replay_stop(opcode, why); }
        if (t->has_view && (t->view.set_format || t->view.set_target || t->view.set_swizzle || (t->view.set_levels && t->view.levels > 1))) {
            snprintf(why, sizeof why, "image slot %d: texture view other than one level / a layer range (serial %llu)", slot, (unsigned long long)t->texture.serial); replay_stop(opcode, why); }
        // Images live on the GPU (they are written there). Reading one whose contents exist only in pool memory would need
        // an upload into the writable texture first: refused until a kernel does it.
        if ((r->img[i].access & 1) && !wonder_host_render_target_existing(&t->texture)) {
            snprintf(why, sizeof why, "image slot %d read before the GPU wrote texture serial %llu (CPU-to-image upload not implemented)", slot, (unsigned long long)t->texture.serial); replay_stop(opcode, why); }
        x.img[slot].image = (struct wonder_nvn_texture_snapshot){.texture = t->texture, .view = t->view, .has_view = t->has_view, .valid = 1};
        x.img[slot].host = target_texture(&x.img[slot].image, opcode); x.img_mask |= (uint8_t)(1u << slot);
    }
    if (call.indirect_address) { x.indirect = 1; x.indirect_span = span_of(opcode, call.indirect_address, 12);
                                 if (call.indirect_address & 3) replay_stop(opcode, "indirect dispatch arguments not 4-byte aligned"); }
    else {
        memcpy(x.groups, call.groups, sizeof x.groups);
        if (!x.groups[0] || !x.groups[1] || !x.groups[2]) return;   // an empty grid runs nothing
    }
    if (wonder_host_dispatch(&x)) replay_stop(opcode, "host compute encoding failed");
    submit_dispatches++;
    static _Atomic unsigned logged;
    if (atomic_fetch_add(&logged, 1) < 16)
        wonder_nvn_report("NVN_DISPATCH_EXECUTED %s cs=%s groups=%d,%d,%d%s local=%ux%ux%u (submit %u command %u)", call.name, x.key, x.groups[0], x.groups[1], x.groups[2],
                          x.indirect ? " (indirect)" : "", r->local[0], r->local[1], r->local[2], state.submits, state.commands);
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
        // The called commands as captured when CallCommands was recorded (see nvnCommandBufferCallCommands).
        unsigned depth = (unsigned)(uintptr_t)context + 1; (void)blob;
        if (depth > 8) wonder_nvn_stop("NVN_INVALID_COMMAND CallCommands nesting deeper than 8");
        wonder_nvn_call_walk(c, replay_visit, (void *)(uintptr_t)depth);
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
    case WONDER_NVN_CMD_SET_POLYGON_OFFSET_CLAMP: {
        uint32_t f = (uint32_t)c->a[0], u = (uint32_t)c->a[1], k = (uint32_t)c->a[2];
        memcpy(&raster.offset_factor, &f, 4); memcpy(&raster.offset_units, &u, 4); memcpy(&raster.offset_clamp, &k, 4); break; }
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
        // Records whose pending writes are report values of this submit are known without the GPU (round199: the
        // finish per conditional, ~50 per frame, serialised CPU and GPU).
        // Off by default (round202: the world map flickered with it on, so some GPU write to these records is not tracked,
        // e.g. a virtual-pool alias of the same memory). WONDER_FAST_CONDITIONAL=1 enables it for study; with it on,
        // WONDER_VERIFY_CONDITIONAL=1 finishes anyway and counts mismatches.
        static int verify = -1; if (verify < 0) { const char *f = getenv("WONDER_FAST_CONDITIONAL"), *e = getenv("WONDER_VERIFY_CONDITIONAL"); verify = !(f && *f == '1') || (e && *e == '1'); }
        static const unsigned words[4] = {0, 4, 16, 20}; uint32_t known[4]; int kind[4], shadowed = 1;
        for (int w = 0; w < 4; w++) { kind[w] = wonder_report_shadow_lookup(&report_shadow, c->a[1] + words[w], &known[w]); if (kind[w] < 0) shadowed = 0; }
        if (!shadowed || verify) replay_finish();
        struct wonder_nvn_pool_view v; wonder_nvn_address_span(c->a[1], 32, &v);
        uint64_t a, b; memcpy(&a, v.memory, 8); memcpy(&b, v.memory + 16, 8);
        if (shadowed) {
            uint32_t m[4]; memcpy(&m[0], &a, 4); memcpy(&m[1], (uint8_t *)&a + 4, 4); memcpy(&m[2], &b, 4); memcpy(&m[3], (uint8_t *)&b + 4, 4);
            for (int w = 0; w < 4; w++) if (kind[w] == 1) m[w] = known[w];   // pending value writes; the rest is current in memory
            uint64_t sa = (uint64_t)m[1] << 32 | m[0], sb = (uint64_t)m[3] << 32 | m[2];
            if (verify && (sa != a || sb != b) && conditional_mismatches++ < 16)
                wonder_nvn_report("NVN_CONDITIONAL_SHADOW_MISMATCH address=%#llx shadow=%llu,%llu memory=%llu,%llu (submit %u)", (unsigned long long)c->a[1],
                                  (unsigned long long)sa, (unsigned long long)sb, (unsigned long long)a, (unsigned long long)b, state.submits);
            a = sa; b = sb; conditional_shadowed++;
        }
        state.render_enable = c->a[0] == 0 ? a == b : a != b;
        conditional_evals++; if (!state.render_enable) conditional_skips++;
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
        { unsigned f = atomic_load(&frame_index) + 1;
          if (draw_log_frame(f)) wonder_nvn_report("NVN_CLEAR_LOG present=%u rt%d=%llu:%ux%u:%#x mask=%#x rgba=%g,%g,%g,%g", f, index, (unsigned long long)t->texture.serial,
                                                   t->texture.desc.width, t->texture.desc.height, t->texture.desc.format, mask, rgba[0], rgba[1], rgba[2], rgba[3]); }
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
        wonder_report_shadow_unknown(&report_shadow, c->a[0], c->a[1]);
        break;
    }
    case WONDER_NVN_CMD_UPDATE_UNIFORM_BUFFER: {
        uint64_t address = c->a[0] + c->a[2]; size_t bytes = c->blob_size;
        if (ubo_update_count < 64) { ubo_updates[ubo_update_count].address = address; ubo_updates[ubo_update_count++].size = bytes; } else ubo_update_overflow = 1;
        if (bytes) wonder_report_shadow_unknown(&report_shadow, address, bytes);
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
        if (timestamp) wonder_report_shadow_unknown(&report_shadow, c->a[1], 16); else wonder_report_shadow_value(&report_shadow, c->a[1], (uint32_t)c->a[0]);
        wonder_host_after_gpu(write_report, r);
        break;
    }
    case WONDER_NVN_CMD_RESET_COUNTER: replay_stop(opcode, "counter reset (occlusion/statistics counters not implemented)");
    // Draw state (snapshotted at record time; kept for draws and the census).
    case WONDER_NVN_CMD_BIND_PROGRAM: {
        // SDK nvnCommandBufferBindProgram (sdk+0x4672f0): when stageBits & the program's stages include COMPUTE (bit 5;
        // stage bits are 1 << NVNshaderStage) only the compute stage is bound; otherwise the program's whole graphics part
        // replaces the graphics stages. Either way the other part stays bound. A NULL program unbinds the stages in stageBits.
        struct wonder_nvn_program_shaders p; memcpy(&p, blob, sizeof p);
        unsigned bits = (unsigned)c->a[0], take;
        if (!c->a[1]) { p.mask = 0; take = bits & 0x3f; }
        else take = (bits & p.mask & 0x20) ? 0x20 : 0x1f;
        for (unsigned st = 0; st < 6; st++) if (take >> st & 1) {
            if (p.mask >> st & 1) { draw_state.program.code[st] = p.code[st]; draw_state.program.control[st] = p.control[st];
                                    draw_state.program.extra[st] = p.extra[st]; memcpy(draw_state.program.key[st], p.key[st], 17); draw_state.program.mask |= 1u << st; }
            else { draw_state.program.code[st] = 0; draw_state.program.control[st] = NULL; draw_state.program.extra[st] = 0; draw_state.program.key[st][0] = 0; draw_state.program.mask &= ~(1u << st); }
        }
        draw_state.program.serial = p.serial; draw_state.has_program = draw_state.program.mask != 0;
        {   // Diagnostic (WONDER_TRACE_VS): replayed binds of listed vertex shaders, and whether a draw follows (draw() logs it)
            const char *list = getenv("WONDER_TRACE_VS");
            if (list && *list && (draw_state.program.mask & 1)) {
                const char *vs = draw_state.program.key[0]; trace_replay_vs = 0;
                for (const char *q = list; *q; ) { size_t n = strcspn(q, ","); if (n && !strncmp(q, vs, n)) trace_replay_vs = 1; q += n; if (*q == ',') q++; }
                static _Atomic unsigned logged;
                if (trace_replay_vs && atomic_fetch_add(&logged, 1) < 200)
                    wonder_nvn_report("NVN_TRACE_REPLAY_BIND vs=%s submit=%u command=%u render_enable=%d", vs, state.submits, state.commands, state.render_enable);
            } else trace_replay_vs = 0;
        }
        break;
    }
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
    case WONDER_NVN_CMD_BIND_IMAGE: draw_state.image_handle[c->a[0]][c->a[1]] = c->a[2]; break;
    // Pure state not consumed by draws yet.
    case WONDER_NVN_CMD_SET_ALPHA_REF: case WONDER_NVN_CMD_SET_CONSERVATIVE_RASTER:
    case WONDER_NVN_CMD_SET_DEPTH_BOUNDS:
    case WONDER_NVN_CMD_SET_PATCH_SIZE: case WONDER_NVN_CMD_SET_POINT_SIZE:
    case WONDER_NVN_CMD_SET_SHADER_SCRATCH: case WONDER_NVN_CMD_SET_STENCIL_MASK: case WONDER_NVN_CMD_SET_STENCIL_REF:
    case WONDER_NVN_CMD_SET_STENCIL_VALUE_MASK:
        break;
    // TX1 performance hints without visible output; Metal orders hazards on tracked resources itself.
    case WONDER_NVN_CMD_SET_TILED_CACHE_ACTION: case WONDER_NVN_CMD_SET_TILED_CACHE_TILE_SIZE:
    case WONDER_NVN_CMD_SET_COLOR_REDUCTION_ENABLE: case WONDER_NVN_CMD_SET_COLOR_REDUCTION_THRESHOLDS: case WONDER_NVN_CMD_BARRIER:
        break;
    case WONDER_NVN_CMD_DRAW_ARRAYS: case WONDER_NVN_CMD_DRAW_ARRAYS_INSTANCED: case WONDER_NVN_CMD_DRAW_ELEMENTS_BASE_VERTEX:
    case WONDER_NVN_CMD_DRAW_ELEMENTS_INDIRECT: case WONDER_NVN_CMD_DRAW_ELEMENTS_INSTANCED: {
        if (trace_replay_vs) { static _Atomic unsigned logged; if (atomic_fetch_add(&logged, 1) < 200)
            wonder_nvn_report("NVN_TRACE_REPLAY_DRAW %s count=%llu render_enable=%d submit=%u", wonder_nvn_command_name(opcode), (unsigned long long)c->a[2], state.render_enable, state.submits); }
        if (!state.render_enable) { disabled_draws++; break; }
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
        // Whether NVN's render enable (conditional rendering) also discards dispatches is not established: stop by name.
        if (!state.render_enable) replay_stop(opcode, "dispatch while rendering is disabled (render enable semantics for compute not established)");
        dispatch(opcode, c);
        break;
    case WONDER_NVN_CMD_COPY_TEXTURE_TO_TEXTURE: {
        struct { struct wonder_nvn_texture_snapshot s, d; struct wonder_nvn_copy_region sr, dr; } blob;
        if (c->blob_size != sizeof blob) replay_stop(opcode, "malformed copy record");
        memcpy(&blob, wonder_nvn_cmd_blob(c), sizeof blob);
        char why[200];
        if (wonder_host_copy_texture(&blob.s, &blob.sr, &blob.d, &blob.dr, (int)c->a[0], why, sizeof why)) replay_stop(opcode, why);
        break;
    }
    case WONDER_NVN_CMD_CLEAR_TEXTURE: case WONDER_NVN_CMD_COPY_TEXTURE_TO_BUFFER:
    case WONDER_NVN_CMD_DOWNSAMPLE:
        replay_stop(opcode, "texture upload/coherence path not implemented");
    default:
        replay_stop(opcode, "unknown command");
    }
}

struct submit_job { void *queue; int count; const uint64_t *handles; };
static void submit_on_replay_stack(void *arg);
void wonder_nvnQueueSubmitCommands(void *queue, int count, const uint64_t *handles) {
    { uint64_t waited = wonder_gpu_gate_wait();   // app not active: iOS refuses GPU work in the background (gpu_gate.h)
      if (waited) wonder_nvn_report("NVN_GPU_GATE submit waited %.1f s for the app to become active", waited / 1e9); }
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
    if (census_sweep() && missing_list[0]) {   // programs first met in this submit (missing_pairs dedups over the run)
        wonder_nvn_report("NVN_CENSUS_SWEEP submit=%u new programs: %s (total %u, skipped draws %u)", state.submits, missing_list, missing_programs, sweep_skipped);
        missing_list[0] = 0;
    }
    if (first_draw_pending) { first_draw_pending = 0; first_draw_captured = 1; wonder_host_capture_first_draw(&first_draw_target); }
    draw_dump_flush();
    if (dump_count) {
        char name[80]; unsigned f = atomic_load(&frame_index) + 1;
        for (int i = 0; i < dump_count; i++) {
            snprintf(name, sizeof name, "FrameDump/f%u-s%u-%d-t%llu.png", f, state.submits, i, (unsigned long long)dump_targets[i].texture.serial);
            wonder_host_capture_target(&dump_targets[i], name);
        }
        dump_count = 0;
    }
    if (text_pending) {
        static int captured; char name[40];
        for (int i = 0; i < text_pending && i < 3; i++) { snprintf(name, sizeof name, "Text/text-%d.png", captured++); wonder_host_capture_target(&text_targets[i], name); }
        text_pending = 0;
    }
    // Diagnostic (WONDER_SUBMIT_SYNC=1): wait for the GPU after every submit, to tell CPU/GPU memory races from
    // wrong rendering.
    { static int sync = -1; if (sync < 0) { const char *e = getenv("WONDER_SUBMIT_SYNC"); sync = e && *e == '1'; }
      if (sync) wonder_host_finish(); }
    unsigned submits = state.submits, commands = state.commands - before; clears = state.clears - clears;
    atomic_store(&replay_owner, (pthread_t *)NULL);
    wonder_host_note_submit(submits);
    if (submits % 600 == 0) wonder_nvn_report("NVN_RENDER_ENABLE submit=%u disabled_draws=%u conditional=%u skip=%u shadowed=%u shadow_mismatches=%u", submits, disabled_draws, conditional_evals, conditional_skips, conditional_shadowed, conditional_mismatches);
    if (submits <= 8 || submits % 600 == 0 || submit_programs[0])
        wonder_nvn_report("NVN_QUEUE_SUBMIT queue=%p submit=%u handles=%d commands=%u clears=%u draws=%u dispatches=%u programs=%s", queue, submits, count, commands, clears,
                          submit_draws, submit_dispatches, submit_programs[0] ? submit_programs : "-");
    submit_draws = 0; submit_dispatches = 0; submit_programs[0] = 0;
}
void wonder_nvnQueueFlush(void *queue) { wonder_nvn_queue_check(queue, "nvnQueueFlush"); wonder_host_flush(); }
void wonder_nvnQueueFinish(void *queue) { wonder_nvn_queue_check(queue, "nvnQueueFinish"); wonder_host_finish(); }

const struct wonder_nvn_entry wonder_nvn_queue_entries[] = {
    {"nvnQueueSubmitCommands", (void *)wonder_nvnQueueSubmitCommands},
    {"nvnQueueFlush", (void *)wonder_nvnQueueFlush},
    {"nvnQueueFinish", (void *)wonder_nvnQueueFinish},
};
const unsigned wonder_nvn_queue_count = sizeof wonder_nvn_queue_entries / sizeof *wonder_nvn_queue_entries;
