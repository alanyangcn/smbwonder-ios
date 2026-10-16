#include "nvn_draw_census.h"
#include <CommonCrypto/CommonDigest.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *const stage_names[6] = {"vs", "fs", "gs", "tcs", "tes", "cs"};
void wonder_nvn_shader_key(const void *control, char key[17]) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(control, WONDER_NVN_SHADER_CONTROL_SIZE, digest);
    for (int i = 0; i < 8; i++) snprintf(key + 2 * i, 3, "%02x", digest[i]);
}
struct out { char *data; size_t size, cap; int failed; };
static void put(struct out *o, const char *format, ...) {
    va_list a; va_start(a, format); va_list b; va_copy(b, a);
    int n = vsnprintf(NULL, 0, format, a); va_end(a);
    if (n < 0) { o->failed = 1; va_end(b); return; }
    if (o->size + (size_t)n + 1 > o->cap) {
        size_t cap = (o->cap ? o->cap * 2 : 4096); while (cap < o->size + (size_t)n + 1) cap *= 2;
        char *p = realloc(o->data, cap); if (!p) { o->failed = 1; va_end(b); return; }
        o->data = p; o->cap = cap;
    }
    vsnprintf(o->data + o->size, (size_t)n + 1, format, b); va_end(b); o->size += (size_t)n;
}
static void texture_json(struct out *o, const struct wonder_nvn_texture *t, const struct wonder_nvn_texture_view *v, int has_view) {
    put(o, "{\"serial\":%llu,\"target\":%d,\"format\":%d,\"width\":%u,\"height\":%u,\"depth\":%u,\"levels\":%u,\"samples\":%d,\"stride\":%u,\"packaged_block_height\":%d,\"swizzle\":[%u,%u,%u,%u]",
        (unsigned long long)t->serial, t->desc.target, t->desc.format, t->desc.width, t->desc.height, t->desc.depth, t->desc.levels, t->desc.samples,
        t->desc.stride, t->desc.pkg_block_h, t->swizzle[0], t->swizzle[1], t->swizzle[2], t->swizzle[3]);
    if (has_view) put(o, ",\"view\":{\"target\":%u,\"format\":%d,\"base_level\":%u,\"levels\":%u,\"min_layer\":%d,\"layers\":%d,\"set\":%u}",
                      v->target, v->format, v->base_level, v->levels, v->min_layer, v->layers,
                      v->set_levels | v->set_layers << 1 | v->set_format << 2 | v->set_swizzle << 3 | v->set_depth_stencil << 4 | v->set_target << 5);
    put(o, "}");
}
static int write_stage(const char *dir, unsigned index, const char *kind, const void *data, size_t size) {
    char name[96]; snprintf(name, sizeof name, "%s/stage-%u.%s.bin", dir, index, kind);
    return wonder_host_census_write(name, data, size);
}
// One census per program (vertex+fragment key pair) per run; the first 256 programs are remembered.
static char written_dirs[256][40]; static unsigned written_count;
static int already_written(const char *dir) { for (unsigned i = 0; i < written_count; i++) if (!strcmp(written_dirs[i], dir)) return 1; return 0; }
int wonder_nvn_draw_census(const struct wonder_nvn_draw_state *s, const struct wonder_nvn_draw_call *d, char *summary, size_t summary_size) {
    unsigned mask = s->has_program ? s->program.mask & (d->compute ? 0x20u : 0x1fu) : 0;   // a draw's graphics stages, or a dispatch's compute stage
    if (!mask) { snprintf(summary, summary_size, "census=none (no %s program bound)", d->compute ? "compute" : "graphics"); return 0; }
    char keys[6][17] = {{0}}, dir[40];
    for (unsigned stage = 0; stage < 6; stage++) if (mask & (1u << stage)) wonder_nvn_shader_key(s->program.control[stage], keys[stage]);
    if (d->compute) snprintf(dir, sizeof dir, "cs-%s", keys[5]);
    else snprintf(dir, sizeof dir, "%s-%s", keys[0][0] ? keys[0] : "none", keys[1][0] ? keys[1] : "none");
    if (already_written(dir)) { snprintf(summary, summary_size, "census=DrawCensus/%s (written earlier this run)", dir); return 0; }
    struct out o = {0}; int failed = 0; unsigned index = 0;
    put(&o, "{\"draw\":{\"name\":\"%s\",\"primitive\":%d,\"index_type\":%d,\"count\":%d,\"first\":%d,\"base_vertex\":%d,\"instances\":%d,\"base_instance\":%d,\"index_address\":%llu,\"indirect_address\":%llu},",
        d->name, d->primitive, d->index_type, d->count, d->first, d->base_vertex, d->instances, d->base_instance,
        (unsigned long long)d->index_address, (unsigned long long)d->indirect_address);
    if (d->compute) {
        const uint8_t *control = s->program.control[5]; uint32_t local[3], shared;
        memcpy(local, control + WONDER_NVN_CONTROL_LOCAL_SIZE, 12); memcpy(&shared, control + WONDER_NVN_CONTROL_SHARED_SIZE, 4);
        put(&o, "\"dispatch\":{\"groups\":[%d,%d,%d],\"local\":[%u,%u,%u],\"shared\":%u},", d->groups[0], d->groups[1], d->groups[2], local[0], local[1], local[2], shared);
    }
    int depth_mode, window_origin; wonder_nvn_device_modes(&depth_mode, &window_origin);
    put(&o, "\"depth_mode\":%d,\"window_origin\":%d,\"program\":{\"serial\":%llu,\"stages\":[", depth_mode, window_origin, (unsigned long long)s->program.serial);
    for (unsigned stage = 0; stage < 6; stage++) {
        if (!(mask & (1u << stage))) continue;
        const uint8_t *control = s->program.control[stage]; uint32_t length, c1_offset;
        memcpy(&c1_offset, control + 0x700, 4); memcpy(&length, control + 0x704, 4);
        struct wonder_nvn_pool_view view = {0};
        if (length) wonder_nvn_address_span(s->program.code[stage], length, &view);
        if (!length || view.size < length || !view.memory) { failed = 1; break; }
        if (write_stage(dir, index, "control", control, WONDER_NVN_SHADER_CONTROL_SIZE) || write_stage(dir, index, "code", view.memory, length)) { failed = 1; break; }
        put(&o, "%s{\"index\":%u,\"stage\":%u,\"name\":\"%s\",\"key\":\"%s\",\"code_address\":%llu,\"code_size\":%u,\"c1_offset\":%u}", index ? "," : "",
            index, stage, stage_names[stage], keys[stage], (unsigned long long)s->program.code[stage], length, c1_offset);
        index++;
    }
    put(&o, "]},\"attribs\":[");
    for (int i = 0; i < s->nattrib; i++)
        put(&o, "%s{\"location\":%d,\"format\":%u,\"offset\":%u,\"stream\":%u}", i ? "," : "", i, s->attrib[i].fmt, s->attrib[i].offset, s->attrib[i].stream);
    put(&o, "],\"streams\":[");
    for (int i = 0; i < s->nstream; i++)
        put(&o, "%s{\"index\":%d,\"stride\":%u,\"divisor\":%u,\"address\":%llu,\"size\":%llu}", i ? "," : "", i, s->stream[i].stride, s->stream[i].divisor,
            (unsigned long long)s->vertex_buffer[i].address, (unsigned long long)s->vertex_buffer[i].size);
    put(&o, "],\"uniform_buffers\":["); int first = 1;
    for (int stage = 0; stage < 6; stage++) for (int i = 0; i < 16; i++) if (s->uniform_buffer[stage][i].address) {
        put(&o, "%s{\"stage\":%d,\"index\":%d,\"size\":%llu,\"address\":%llu}", first ? "" : ",", stage, i,
            (unsigned long long)s->uniform_buffer[stage][i].size, (unsigned long long)s->uniform_buffer[stage][i].address); first = 0;
    }
    put(&o, "],\"textures\":["); first = 1;
    for (int stage = 0; stage < 6; stage++) for (int i = 0; i < 32; i++) if (s->texture_handle[stage][i]) {
        uint64_t h = s->texture_handle[stage][i]; int texture_id = (int)(h & 0xfffff), sampler_id = (int)((h >> 20) & 0xfff);
        put(&o, "%s{\"stage\":%d,\"index\":%d,\"texture_id\":%d,\"sampler_id\":%d,\"handle\":%llu", first ? "" : ",", stage, i, texture_id, sampler_id, (unsigned long long)h); first = 0;
        const struct wonder_nvn_texture_registration *r = s->texture_pool ? wonder_nvn_texture_lookup(s->texture_pool, texture_id) : NULL;
        if (r && r->valid) { put(&o, ",\"texture\":"); texture_json(&o, &r->texture, &r->view, r->has_view); } else put(&o, ",\"texture\":null");
        const struct wonder_nvn_sampler_state *m = s->sampler_pool ? wonder_nvn_sampler_lookup(s->sampler_pool, sampler_id) : NULL;
        if (m && m->valid) put(&o, ",\"sampler\":{\"min\":%u,\"mag\":%u,\"wrap\":[%u,%u,%u],\"compare_mode\":%u,\"compare_func\":%u,\"reduction\":%u,\"lod\":[%g,%g,%g],\"anisotropy\":%g,\"border\":[%g,%g,%g,%g]}}",
                               m->min, m->mag, m->wrap_s, m->wrap_t, m->wrap_r, m->compare_mode, m->compare_func, m->reduction, m->lod_min, m->lod_max, m->lod_bias,
                               m->anisotropy, m->border[0], m->border[1], m->border[2], m->border[3]);
        else put(&o, ",\"sampler\":null}");
    }
    put(&o, "],\"images\":["); first = 1;
    for (int stage = 0; stage < 6; stage++) for (int i = 0; i < 8; i++) if (s->image_handle[stage][i]) {
        uint64_t h = s->image_handle[stage][i];
        put(&o, "%s{\"stage\":%d,\"index\":%d,\"image_id\":%llu", first ? "" : ",", stage, i, (unsigned long long)h); first = 0;
        const struct wonder_nvn_texture_registration *r = s->texture_pool && h < (1u << 20) ? wonder_nvn_texture_lookup(s->texture_pool, (int)h) : NULL;
        if (r && r->valid) { put(&o, ",\"texture\":"); texture_json(&o, &r->texture, &r->view, r->has_view); put(&o, "}"); } else put(&o, ",\"texture\":null}");
    }
    put(&o, "],\"targets\":{\"colors\":[");
    for (int i = 0; s->has_targets && i < s->color_count; i++) {
        put(&o, "%s", i ? "," : "");
        if (s->targets[i].valid) texture_json(&o, &s->targets[i].texture, &s->targets[i].view, s->targets[i].has_view); else put(&o, "null");
    }
    put(&o, "],\"depth\":");
    if (s->has_targets && s->targets[s->color_count].valid) texture_json(&o, &s->targets[s->color_count].texture, &s->targets[s->color_count].view, s->targets[s->color_count].has_view);
    else put(&o, "null");
    put(&o, "},\"blend\":[");
    for (int i = 0, n = 0; i < 8; i++) if (s->blend_set & (1u << i)) {
        const wonder_nvn_blend_state *b = &s->blend[i];
        put(&o, "%s{\"target\":%d,\"src_rgb\":%u,\"dst_rgb\":%u,\"src_a\":%u,\"dst_a\":%u,\"eq_rgb\":%u,\"eq_a\":%u}", n++ ? "," : "", i,
            (unsigned)b->src_rgb, (unsigned)b->dst_rgb, (unsigned)b->src_a, (unsigned)b->dst_a, (unsigned)b->eq_rgb, (unsigned)b->eq_a);
    }
    put(&o, "],\"color\":");
    if (s->states_set & 1) put(&o, "{\"blend_enable\":%u,\"logic_op\":%u,\"alpha_func\":%u,\"alpha_ref_bits\":%u}", s->color.blend_en, s->color.logic_op, s->color.alpha_func, s->color.alpha_ref_bits); else put(&o, "null");
    put(&o, ",\"channel_mask\":"); if (s->states_set & 2) put(&o, "%u", s->channel_mask.mask); else put(&o, "null");
    put(&o, ",\"depth_stencil\":");
    if (s->states_set & 4) { const wonder_nvn_depth_stencil_state *z = &s->depth_stencil;
        put(&o, "{\"depth_test\":%u,\"depth_write\":%u,\"depth_func\":%u,\"stencil_test\":%u,\"stencil_func\":[%u,%u],\"front_ops\":[%u,%u,%u],\"back_ops\":[%u,%u,%u]}",
            (unsigned)z->dt, (unsigned)z->dw, (unsigned)z->df, (unsigned)z->st, (unsigned)z->sf_f, (unsigned)z->sf_b, (unsigned)z->fail_f, (unsigned)z->zfail_f,
            (unsigned)z->zpass_f, (unsigned)z->fail_b, (unsigned)z->zfail_b, (unsigned)z->zpass_b); } else put(&o, "null");
    put(&o, ",\"polygon\":"); if (s->states_set & 8) put(&o, "{\"cull\":%u,\"front\":%u,\"mode\":%u,\"offset_enable\":%u}", s->polygon.cull, s->polygon.front, s->polygon.mode, s->polygon.offset_en); else put(&o, "null");
    put(&o, ",\"multisample\":"); if (s->states_set & 16) put(&o, "{\"enable\":%u,\"samples\":%u,\"alpha_to_coverage\":%u}", s->multisample.enable, s->multisample.samples, s->multisample.a2c); else put(&o, "null");
    put(&o, ",\"viewport\":"); if (s->viewport_set) put(&o, "[%g,%g,%g,%g]", s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]); else put(&o, "null");
    put(&o, ",\"scissor\":"); if (s->scissor_set) put(&o, "[%d,%d,%d,%d]", s->scissor[0], s->scissor[1], s->scissor[2], s->scissor[3]); else put(&o, "null");
    put(&o, "}\n");
    char json_name[64]; snprintf(json_name, sizeof json_name, "%s/draw.json", dir);
    if (!failed && !o.failed && !wonder_host_census_write(json_name, o.data, o.size)) {
        if (written_count < 256) snprintf(written_dirs[written_count++], sizeof written_dirs[0], "%s", dir);
        static char index[256 * 41 + 1]; size_t at = 0;   // (static: off the caller's stack) index.txt: every census directory of this run, one per line (device fetch)
        for (unsigned i = 0; i < written_count; i++) at += (size_t)snprintf(index + at, sizeof index - at, "%s\n", written_dirs[i]);
        wonder_host_census_write("index.txt", index, at);
        if (d->compute) snprintf(summary, summary_size, "census=DrawCensus/%s cs=%s", dir, keys[5]);
        else snprintf(summary, summary_size, "census=DrawCensus/%s vs=%s fs=%s", dir, keys[0][0] ? keys[0] : "-", keys[1][0] ? keys[1] : "-");
        free(o.data); return 1;
    }
    free(o.data);
    snprintf(summary, summary_size, "census=FAILED (stage code unresolved or write failed)");
    return -1;
}
