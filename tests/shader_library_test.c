// .refl parsing (tools/wonder_shader_build.py output): every line kind, limits and malformed input.
#include "shader_library.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int parse(const char *text, struct wonder_shader_refl *r, char *err) { return wonder_shader_refl_parse(text, strlen(text), r, err, 128); }
int main(void) {
    struct wonder_shader_refl r; char err[128];
    // Round53 vertex and fragment reflections.
    assert(parse("stage vs\ncbuf 0 1\nattr 0 0 float\n", &r, err) == 0);
    assert(r.stage == 0 && r.ncbuf == 1 && r.cbuf[0].slot == 0 && r.cbuf[0].buffer == 1 && r.nattr == 1 &&
           r.attr[0].location == 0 && r.attr[0].nvn == 0 && r.attr[0].type == WONDER_SHADER_FLOAT && r.ntex == 0 && r.support < 0 && r.c1_buffer < 0);
    assert(parse("stage ps\ntex 0 1 1 2d\nfout 0 float\n", &r, err) == 0);
    assert(r.stage == 1 && r.ntex == 1 && r.tex[0].slot == 0 && r.tex[0].texture == 1 && r.tex[0].sampler == 1 &&
           r.tex[0].type == WONDER_SHADER_TEX_2D && r.fout_mask == 1 && r.fout_type[0] == WONDER_SHADER_FLOAT);
    // c1 constants, support buffer, driver segment reads, integer types.
    assert(parse("stage ps\nc1 2 0011ff\nsupport 0\ndrv 2\nfout 1 uint\nattr 3 3 sint\n", &r, err) == 0);
    assert(r.c1_buffer == 2 && r.c1_len == 3 && r.c1[0] == 0 && r.c1[2] == 0xff && r.support == 0 && (r.drv_read & 4) &&
           r.fout_mask == 2 && r.fout_type[1] == WONDER_SHADER_UINT && r.attr[0].type == WONDER_SHADER_SINT);
    assert(parse("stage vs\nc1 2 -\n", &r, err) == 0 && r.c1_buffer == 2 && r.c1_len == 0);
    // Vertex depth convention the stage was built for.
    assert(parse("stage vs\ndepth minus_one\n", &r, err) == 0 && r.depth_minus_one == 1);
    assert(parse("stage vs\ndepth zero\n", &r, err) == 0 && r.depth_minus_one == 0);
    assert(parse("stage vs\n", &r, err) == 0 && r.depth_minus_one == -1);
    assert(parse("stage vs\ndepth sideways\n", &r, err) && strstr(err, "depth"));
    // Rejections name the problem.
    assert(parse("stage gs\n", &r, err) && strstr(err, "stage"));
    assert(parse("cbuf 0 1\n", &r, err) && strstr(err, "stage"));                  // stage line first
    assert(parse("stage vs\nfrob 1\n", &r, err) && strstr(err, "frob"));
    assert(parse("stage vs\nc1 2 abc\n", &r, err) && strstr(err, "c1"));          // odd hex length
    assert(parse("stage vs\ncbuf 14 1\n", &r, err) && strstr(err, "cbuf"));       // NVN uniform slots 0..13
    assert(parse("stage vs\ncbuf 0 16\n", &r, err) && strstr(err, "buffer"));     // buffers 16..30 are vertex streams
    assert(parse("stage ps\ntex 32 1 1 2d\n", &r, err) && strstr(err, "tex"));
    assert(parse("stage ps\ntex 0 1 1 hexagon\n", &r, err) && strstr(err, "hexagon"));
    char many[2048] = "stage vs\n"; for (int i = 0; i < 17; i++) snprintf(many + strlen(many), sizeof many - strlen(many), "attr %d %d float\n", i, i);
    assert(parse(many, &r, err) && strstr(err, "attr"));
    char cbufs[2048] = "stage vs\n"; for (int i = 0; i < 15; i++) snprintf(cbufs + strlen(cbufs), sizeof cbufs - strlen(cbufs), "cbuf %d %d\n", i % 14, i % 14);
    assert(parse(cbufs, &r, err) && strstr(err, "cbuf"));
    puts("shader_library OK");
    return 0;
}
