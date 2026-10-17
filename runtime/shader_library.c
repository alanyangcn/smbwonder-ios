#include "shader_library.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static int fail(char *err, size_t n, const char *format, ...) { va_list a; va_start(a, format); vsnprintf(err, n, format, a); va_end(a); return 1; }
static int type_of(const char *s) { return !strcmp(s, "float") ? WONDER_SHADER_FLOAT : !strcmp(s, "uint") ? WONDER_SHADER_UINT : !strcmp(s, "sint") ? WONDER_SHADER_SINT : -1; }
static int texture_type_of(const char *s) {
    static const char *const names[] = {"1d", "2d", "3d", "1darray", "2darray", "2dms", "2dmsarray", "cube", "cubearray", "buffer"};
    for (int i = 0; i < 10; i++) if (!strcmp(s, names[i])) return i;
    return -1;
}
static int hex_value(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
int wonder_shader_format_class(int f) {
    switch (f) {
    case 0x3: case 0x8: case 0xB: case 0xF: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23: case 0x27: case 0x2C: case 0x2F:
    case 0x3E: case 0x6F: case 0x74: case 0x77: return WONDER_SHADER_UINT;
    case 0x4: case 0x9: case 0xC: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24: case 0x28: case 0x2D: case 0x30:
    case 0x6A: case 0x70: case 0x75: case 0x78: return WONDER_SHADER_SINT;
    default: return WONDER_SHADER_FLOAT;
    }
}
int wonder_shader_refl_parse(const char *text, size_t size, struct wonder_shader_refl *r, char *err, size_t n) {
    memset(r, 0, sizeof *r); r->stage = -1; r->support = -1; r->smp_overflow = -1; r->c1_buffer = -1; r->depth_minus_one = -1; r->drv0_buffer = -1;
    size_t at = 0; unsigned line_no = 0;
    while (at < size) {
        size_t end = at; while (end < size && text[end] != '\n') end++;
        char line[8300]; size_t len = end - at; line_no++;
        if (len >= sizeof line) return fail(err, n, "line %u too long", line_no);
        memcpy(line, text + at, len); line[len] = 0; at = end + 1;
        if (!len) continue;
        // Buffer bindings: the vertex stage keeps 15 (disabled-attribute defaults) and 16..30 (streams); other stages use 0..30.
        int buffers = r->stage == 0 ? WONDER_SHADER_DEFAULT_ATTRIBUTE_BUFFER : 31;
        char kind[16] = "", word[8200] = ""; int a = -1, b = -1, c = -1;
        if (sscanf(line, "%15s", kind) != 1) return fail(err, n, "line %u empty", line_no);
        if (r->stage < 0 && strcmp(kind, "stage")) return fail(err, n, "line %u: stage line must come first", line_no);
        if (!strcmp(kind, "stage")) {
            if (sscanf(line, "stage %15s", word) != 1 || (strcmp(word, "vs") && strcmp(word, "ps") && strcmp(word, "cs"))) return fail(err, n, "unsupported stage '%s'", word);
            r->stage = !strcmp(word, "ps") ? 1 : !strcmp(word, "cs") ? 5 : 0;
        } else if (!strcmp(kind, "local")) {
            unsigned x, y, z;
            if (r->stage != 5 || sscanf(line, "local %u %u %u", &x, &y, &z) != 3 || !x || !y || !z || (uint64_t)x * y * z > 1024)
                return fail(err, n, "local line %u: compute threadgroup x y z, 1..1024 threads", line_no);
            r->local[0] = x; r->local[1] = y; r->local[2] = z;
        } else if (!strcmp(kind, "shared")) {
            unsigned bytes;
            if (r->stage != 5 || sscanf(line, "shared %u", &bytes) != 1 || bytes > 32768) return fail(err, n, "shared line %u: compute, at most 32 KiB", line_no);
            r->shared = bytes;
        } else if (!strcmp(kind, "uptr")) {
            if (r->stage != 5 || sscanf(line, "uptr %d %d", &a, &b) != 2 || a < 0 || a > 13 || b < 0 || b >= buffers || r->nuptr >= 14)
                return fail(err, n, "uptr line %u: compute, uniform slot 0..13, buffer 0..14", line_no);
            r->uptr[r->nuptr].slot = (uint8_t)a; r->uptr[r->nuptr++].buffer = (uint8_t)b;
        } else if (!strcmp(kind, "img")) {
            char access[8] = "";
            if (r->stage != 5 || sscanf(line, "img %d %d %15s %7s", &a, &b, word, access) != 4 || a < 0 || a > 7 || b < 0 || b > 95 || r->nimg >= 8)
                return fail(err, n, "img line %u: compute, image slot 0..7, texture 0..95, type, r|w|rw", line_no);
            int t = texture_type_of(word); if (t < 0) return fail(err, n, "img line %u: unknown texture type '%s'", line_no, word);
            int acc = !strcmp(access, "r") ? 1 : !strcmp(access, "w") ? 2 : !strcmp(access, "rw") ? 3 : 0;
            if (!acc) return fail(err, n, "img line %u: access must be r, w or rw", line_no);
            r->img[r->nimg].slot = (uint8_t)a; r->img[r->nimg].texture = (uint8_t)b; r->img[r->nimg].type = (uint8_t)t; r->img[r->nimg++].access = (uint8_t)acc;
        } else if (!strcmp(kind, "cbuf")) {
            if (sscanf(line, "cbuf %d %d", &a, &b) != 2 || a < 0 || a > 13) return fail(err, n, "cbuf line %u: NVN uniform slot must be 0..13", line_no);
            if (b < 0 || b >= buffers) return fail(err, n, "cbuf line %u: buffer %d beyond the stage's buffer bindings (vertex 0..14, others 0..30)", line_no, b);
            if (r->ncbuf >= 14) return fail(err, n, "cbuf line %u: more than 14 uniform buffers", line_no);
            r->cbuf[r->ncbuf].slot = (uint8_t)a; r->cbuf[r->ncbuf].extent = 0; r->cbuf[r->ncbuf++].buffer = (uint8_t)b;
        } else if (!strcmp(kind, "cbufext")) {   // round222: bytes of uniform slot r the stage reads (0 = a computed index)
            int i = 0;
            if (sscanf(line, "cbufext %d %d", &a, &b) != 2 || b < 0 || b > 0x10000) return fail(err, n, "cbufext line %u: slot and 0..65536 bytes", line_no);
            while (i < r->ncbuf && r->cbuf[i].slot != a) i++;
            if (i == r->ncbuf) return fail(err, n, "cbufext line %u: uniform slot %d has no cbuf line before it", line_no, a);
            r->cbuf[i].extent = (uint32_t)b;
        } else if (!strcmp(kind, "support")) {
            if (sscanf(line, "support %d", &a) != 1 || a < 0 || a >= buffers) return fail(err, n, "support line %u: bad buffer", line_no);
            r->support = a;
        } else if (!strcmp(kind, "smpov")) {   // samplers 16.. read from an argument buffer (wonder_shader_build.py sampler_overflow)
            if (sscanf(line, "smpov %d", &a) != 1 || a < 0 || a >= buffers) return fail(err, n, "smpov line %u: bad buffer", line_no);
            r->smp_overflow = a;
        } else if (!strcmp(kind, "c1")) {
            if (sscanf(line, "c1 %d %8199s", &a, word) != 2 || a < 0 || a >= buffers) return fail(err, n, "c1 line %u: bad buffer", line_no);
            size_t hex = strcmp(word, "-") ? strlen(word) : 0;
            if (hex % 2 || hex / 2 > sizeof r->c1) return fail(err, n, "c1 line %u: bad hex length %zu", line_no, hex);
            for (size_t i = 0; i < hex / 2; i++) {
                int hi = hex_value(word[2 * i]), lo = hex_value(word[2 * i + 1]);
                if (hi < 0 || lo < 0) return fail(err, n, "c1 line %u: bad hex digit", line_no);
                r->c1[i] = (uint8_t)(hi << 4 | lo);
            }
            r->c1_buffer = a; r->c1_len = (uint16_t)(hex / 2);
        } else if (!strcmp(kind, "depth")) {
            if (sscanf(line, "depth %15s", word) != 1 || (strcmp(word, "minus_one") && strcmp(word, "zero"))) return fail(err, n, "depth line %u: minus_one or zero", line_no);
            r->depth_minus_one = !strcmp(word, "minus_one");
        } else if (!strcmp(kind, "drv")) {
            int got = sscanf(line, "drv %d %d", &a, &b);
            if (got < 1 || (a != 0 && a != 2)) return fail(err, n, "drv line %u: bank must be 0 or 2", line_no);
            if (got == 2) {   // c[0] whose reads the build proved to lie in the buffer address tables: the host fills it
                if (a != 0 || b < 0 || b >= buffers) return fail(err, n, "drv line %u: only c[0] takes a buffer, within the stage's bindings", line_no);
                r->drv0_buffer = b;
            } else r->drv_read |= (uint8_t)(1u << a);
        } else if (!strcmp(kind, "tex")) {
            if (sscanf(line, "tex %d %d %d %15s", &a, &b, &c, word) != 4 || a < 0 || a > 31 || b < 0 || b > 95 || c < 0 || c > 47 || (c > 15 && r->smp_overflow < 0) || r->ntex >= 40)
                return fail(err, n, "tex line %u: slot 0..31, texture 0..95, sampler 0..15 (16..47 after an smpov line)", line_no);
            int t = texture_type_of(word); if (t < 0) return fail(err, n, "tex line %u: unknown texture type '%s'", line_no, word);
            r->tex[r->ntex] = (struct wonder_shader_tex){(uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)t, 0, 0, 0}; r->ntex++;
        } else if (!strcmp(kind, "btex")) {
            int ubo = -1, offset = -1, tx = -1, sm = -1, bindless = 0;
            for (int i = 0; i < r->ntex; i++) bindless += r->tex[i].bindless;
            if (sscanf(line, "btex %d %d %d %d %15s", &ubo, &offset, &tx, &sm, word) != 5 || ubo < 0 || ubo > 13 || offset < 0 || offset > 0x10000 - 4 || (offset & 3) ||
                tx < 0 || tx > 95 || sm < 0 || sm > 47 || (sm > 15 && r->smp_overflow < 0) || r->ntex >= 40 || bindless >= 8)
                return fail(err, n, "btex line %u: uniform slot 0..13, offset within 64 KiB, at most 8", line_no);
            int t = texture_type_of(word); if (t < 0) return fail(err, n, "btex line %u: unknown texture type '%s'", line_no, word);
            r->tex[r->ntex] = (struct wonder_shader_tex){(uint8_t)(32 + bindless), (uint8_t)tx, (uint8_t)sm, (uint8_t)t, 1, (uint8_t)ubo, (uint16_t)offset}; r->ntex++;
        } else if (!strcmp(kind, "sbuf")) {
            if (sscanf(line, "sbuf %d %d %15s", &a, &b, word) != 3 || a < 0 || a > 15 || b < 0 || b >= buffers || r->nsbuf >= 16 ||
                (strcmp(word, "r") && strcmp(word, "rw")))
                return fail(err, n, "sbuf line %u: storage slot 0..15, buffer 0..15, r or rw", line_no);
            r->sbuf[r->nsbuf].slot = (uint8_t)a; r->sbuf[r->nsbuf].buffer = (uint8_t)b; r->sbuf[r->nsbuf++].write = !strcmp(word, "rw");
        } else if (!strcmp(kind, "attr")) {
            if (sscanf(line, "attr %d %d %15s", &a, &b, word) != 3 || a < 0 || a > 15 || b < 0 || b > 15 || r->nattr >= 16)
                return fail(err, n, "attr line %u: location/attribute 0..15, at most 16", line_no);
            int t = type_of(word); if (t < 0) return fail(err, n, "attr line %u: unknown type '%s'", line_no, word);
            r->attr[r->nattr].location = (uint8_t)a; r->attr[r->nattr].nvn = (uint8_t)b; r->attr[r->nattr++].type = (uint8_t)t;
        } else if (!strcmp(kind, "fout")) {
            if (sscanf(line, "fout %d %15s", &a, word) != 2 || a < 0 || a > 7) return fail(err, n, "fout line %u: colour 0..7", line_no);
            int t = type_of(word); if (t < 0) return fail(err, n, "fout line %u: unknown type '%s'", line_no, word);
            r->fout_mask |= (uint8_t)(1u << a); r->fout_type[a] = (uint8_t)t;
        } else return fail(err, n, "line %u: unknown kind '%s'", line_no, kind);
    }
    if (r->stage < 0) return fail(err, n, "no stage line");
    if (r->stage == 5 && !r->local[0]) return fail(err, n, "compute reflection without a local line");
    return 0;
}
#include <stdlib.h>
#include <ctype.h>
struct text { char *p; size_t n, cap; int failed; };
static void append(struct text *t, const char *s, size_t n) {
    if (t->failed) return;
    if (t->n + n + 1 > t->cap) { size_t cap = t->cap ? t->cap * 2 : 4096; while (cap < t->n + n + 1) cap *= 2;
        char *q = realloc(t->p, cap); if (!q) { t->failed = 1; return; } t->p = q; t->cap = cap; }
    memcpy(t->p + t->n, s, n); t->n += n; t->p[t->n] = 0;
}
int wonder_shader_retype_attributes(const char *msl, size_t size, const uint8_t types[32], char **out, char *err, size_t errlen) {
    *out = NULL;
    char names[32][24] = {{0}};   // per location: the stage_in member name (m_N)
    for (int loc = 0; loc < 32; loc++) {
        if (types[loc] != WONDER_SHADER_UINT && types[loc] != WONDER_SHADER_SINT) continue;
        char tag[40]; snprintf(tag, sizeof tag, " [[attribute(%d)]];", loc);
        const char *at = memmem(msl, size, tag, strlen(tag));
        if (!at) return fail(err, errlen, "no [[attribute(%d)]] declaration", loc);
        const char *line = at; while (line > msl && line[-1] != '\n') line--;
        const char *q = line; while (*q == ' ') q++;
        if (strncmp(q, "float4 ", 7)) return fail(err, errlen, "attribute %d is not declared float4", loc);
        q += 7; size_t len = (size_t)(at - q);
        if (!len || len >= sizeof names[loc]) return fail(err, errlen, "attribute %d member name", loc);
        memcpy(names[loc], q, len); names[loc][len] = 0;
    }
    struct text t = {0}; const char *p = msl, *end = msl + size;
    while (p < end) {
        int done = 0;
        for (int loc = 0; loc < 32 && !done; loc++) {
            if (!names[loc][0]) continue;
            size_t len = strlen(names[loc]);
            // declaration: "float4 <name> [[attribute(" -> uint4 / int4
            if ((size_t)(end - p) > 7 + len && !strncmp(p, "float4 ", 7) && !strncmp(p + 7, names[loc], len) && p[7 + len] == ' ') {
                const char *ty = types[loc] == WONDER_SHADER_UINT ? "uint4 " : "int4 "; append(&t, ty, strlen(ty)); p += 7; done = 1; break;
            }
            // read: "in.<name>[.swizzle]" (not a longer member name)
            if ((size_t)(end - p) > 3 + len && !strncmp(p, "in.", 3) && !strncmp(p + 3, names[loc], len) && !isalnum((unsigned char)p[3 + len]) && p[3 + len] != '_') {
                const char *s = p + 3 + len; size_t sw = 0;
                if (*s == '.') { while (s + 1 + sw < end && s[1 + sw] && strchr("xyzwrgba", s[1 + sw])) sw++; }
                if (*s == '.' && !sw) { t.failed = 1; break; }
                size_t comps = sw ? sw : 4; if (comps > 4) { free(t.p); return fail(err, errlen, "attribute %d swizzle", loc); }
                char head[24]; snprintf(head, sizeof head, comps == 1 ? "as_type<float>(" : "as_type<float%zu>(", comps);
                append(&t, head, strlen(head)); append(&t, p, (size_t)(s - p) + (sw ? sw + 1 : 0)); append(&t, ")", 1);
                p = s + (sw ? sw + 1 : 0); done = 1; break;
            }
        }
        if (!done) { append(&t, p, 1); p++; }
    }
    if (t.failed) { free(t.p); return fail(err, errlen, "rewrite failed"); }
    *out = t.p; return 0;
}
