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
    // Bindless texture: handle read from NVN uniform slot 1 at byte 0xd0; reflected as texture slot 32 + n (round86).
    assert(parse("stage ps\ntex 0 1 1 2d\nbtex 1 208 3 3 2d\nfout 0 float\n", &r, err) == 0);
    assert(r.ntex == 2 && r.tex[1].slot == 32 && r.tex[1].texture == 3 && r.tex[1].sampler == 3 && r.tex[1].type == WONDER_SHADER_TEX_2D &&
           r.tex[1].bindless && r.tex[1].ubo_slot == 1 && r.tex[1].offset == 208 && !r.tex[0].bindless);
    assert(parse("stage ps\nbtex 14 0 3 3 2d\n", &r, err) && strstr(err, "btex"));
    assert(parse("stage ps\nbtex 1 65534 3 3 2d\n", &r, err) && strstr(err, "btex"));   // handle must fit in the 64 KiB block
    // Storage buffers: NVN slot r -> device [[buffer(n)]], read-only or read-write.
    assert(parse("stage vs\ndepth minus_one\nsbuf 0 2 r\nsbuf 3 4 rw\n", &r, err) == 0);
    assert(r.nsbuf == 2 && r.sbuf[0].slot == 0 && r.sbuf[0].buffer == 2 && !r.sbuf[0].write && r.sbuf[1].slot == 3 && r.sbuf[1].write);
    assert(parse("stage vs\nsbuf 16 2 r\n", &r, err) && strstr(err, "sbuf"));
    assert(parse("stage vs\nsbuf 0 16 r\n", &r, err) && strstr(err, "sbuf"));
    // Rejections name the problem.
    assert(parse("stage gs\n", &r, err) && strstr(err, "stage"));
    assert(parse("cbuf 0 1\n", &r, err) && strstr(err, "stage"));                  // stage line first
    assert(parse("stage vs\nfrob 1\n", &r, err) && strstr(err, "frob"));
    assert(parse("stage vs\nc1 2 abc\n", &r, err) && strstr(err, "c1"));          // odd hex length
    assert(parse("stage vs\ncbuf 14 1\n", &r, err) && strstr(err, "cbuf"));       // NVN uniform slots 0..13
    assert(parse("stage vs\ncbuf 0 16\n", &r, err) && strstr(err, "buffer"));     // buffers 16..30 are vertex streams
    assert(parse("stage vs\ncbuf 0 15\n", &r, err) && strstr(err, "buffer"));     // buffer 15 holds disabled-attribute defaults
    assert(parse("stage ps\ntex 32 1 1 2d\n", &r, err) && strstr(err, "tex"));
    assert(parse("stage ps\ntex 0 1 1 hexagon\n", &r, err) && strstr(err, "hexagon"));
    char many[2048] = "stage vs\n"; for (int i = 0; i < 17; i++) snprintf(many + strlen(many), sizeof many - strlen(many), "attr %d %d float\n", i, i);
    assert(parse(many, &r, err) && strstr(err, "attr"));
    char cbufs[2048] = "stage vs\n"; for (int i = 0; i < 15; i++) snprintf(cbufs + strlen(cbufs), sizeof cbufs - strlen(cbufs), "cbuf %d %d\n", i % 14, i % 14);
    assert(parse(cbufs, &r, err) && strstr(err, "cbuf"));
    // Compute (round135): threadgroup size, shared memory, uniforms read by address, images.
    assert(!parse("stage cs\nlocal 8 8 1\nshared 1024\ncbuf 0 1\nuptr 2 3\nsbuf 1 4 rw\nimg 0 5 2d w\nimg 1 6 3d rw\n", &r, err));
    assert(r.stage == 5 && r.local[0] == 8 && r.local[1] == 8 && r.local[2] == 1 && r.shared == 1024);
    assert(r.nuptr == 1 && r.uptr[0].slot == 2 && r.uptr[0].buffer == 3 && r.nsbuf == 1 && r.sbuf[0].write);
    assert(r.nimg == 2 && r.img[0].slot == 0 && r.img[0].texture == 5 && r.img[0].type == WONDER_SHADER_TEX_2D && r.img[0].access == 2);
    assert(r.img[1].type == WONDER_SHADER_TEX_3D && r.img[1].access == 3);
    assert(parse("stage cs\nshared 0\n", &r, err) && strstr(err, "local"));            // a kernel needs its threadgroup size
    assert(parse("stage cs\nlocal 64 32 1\n", &r, err) && strstr(err, "local"));        // 2048 threads > 1024
    assert(parse("stage ps\nlocal 1 1 1\n", &r, err) && strstr(err, "local"));          // compute-only lines
    assert(parse("stage cs\nlocal 1 1 1\nimg 0 1 2d x\n", &r, err) && strstr(err, "img"));
    assert(parse("stage cs\nlocal 1 1 1\nshared 40000\n", &r, err) && strstr(err, "shared"));
    puts("shader_library OK");
    return 0;
}
