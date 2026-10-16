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
int wonder_shader_refl_parse(const char *text, size_t size, struct wonder_shader_refl *r, char *err, size_t n) {
    memset(r, 0, sizeof *r); r->stage = -1; r->support = -1; r->c1_buffer = -1; r->depth_minus_one = -1;
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
            r->cbuf[r->ncbuf].slot = (uint8_t)a; r->cbuf[r->ncbuf++].buffer = (uint8_t)b;
        } else if (!strcmp(kind, "support")) {
            if (sscanf(line, "support %d", &a) != 1 || a < 0 || a >= buffers) return fail(err, n, "support line %u: bad buffer", line_no);
            r->support = a;
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
            if (sscanf(line, "drv %d", &a) != 1 || (a != 0 && a != 2)) return fail(err, n, "drv line %u: bank must be 0 or 2", line_no);
            r->drv_read |= (uint8_t)(1u << a);
        } else if (!strcmp(kind, "tex")) {
            if (sscanf(line, "tex %d %d %d %15s", &a, &b, &c, word) != 4 || a < 0 || a > 31 || b < 0 || b > 95 || c < 0 || c > 15 || r->ntex >= 40)
                return fail(err, n, "tex line %u: slot 0..31, texture 0..95, sampler 0..15", line_no);
            int t = texture_type_of(word); if (t < 0) return fail(err, n, "tex line %u: unknown texture type '%s'", line_no, word);
            r->tex[r->ntex] = (struct wonder_shader_tex){(uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)t, 0, 0, 0}; r->ntex++;
        } else if (!strcmp(kind, "btex")) {
            int ubo = -1, offset = -1, tx = -1, sm = -1, bindless = 0;
            for (int i = 0; i < r->ntex; i++) bindless += r->tex[i].bindless;
            if (sscanf(line, "btex %d %d %d %d %15s", &ubo, &offset, &tx, &sm, word) != 5 || ubo < 0 || ubo > 13 || offset < 0 || offset > 0x10000 - 4 || (offset & 3) ||
                tx < 0 || tx > 95 || sm < 0 || sm > 15 || r->ntex >= 40 || bindless >= 8)
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
