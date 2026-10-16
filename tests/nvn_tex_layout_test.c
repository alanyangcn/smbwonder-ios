#include "nvn_tex.h"
#include "nvn_deswizzle.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    // round158: 64x1 RGBA16F (0x29) flagged LINEAR_RENDER_TARGET (0x40) only is pitch layout: 512 bytes per row,
    // rows aligned to 128 bytes, no block-linear deswizzle.
    nvn_texdesc d = {.target = 1, .format = 0x29, .flags = 0x40, .width = 64, .height = 1, .depth = 1, .levels = 1};
    assert(nvn_tex_is_linear(&d));
    nvn_texlayout L; nvn_tex_layout(&d, &L);
    assert(L.size == 512 && L.align == 128 && nvn_tex_level_offset(&d, 0) == 0);
    uint32_t rb, rows, slices, bpb; uint64_t base; unsigned th, td;
    assert(nvn_deswizzle_geom(&d, 0, 0, &rb, &rows, &slices, &base, &th, &td, &bpb) == -1);   // uploaded as rows, not tiles
    d.width = 20; nvn_tex_layout(&d, &L); assert(L.size == 256);   // 160-byte rows padded to 256
    // The same texture block-linear (compressible only): one GOB row of 8 GOBs.
    nvn_texdesc b = {.target = 1, .format = 0x29, .flags = 0x8, .width = 64, .height = 1, .depth = 1, .levels = 1};
    assert(!nvn_tex_is_linear(&b)); nvn_tex_layout(&b, &L); assert(L.size == 4096);
    assert(nvn_deswizzle_geom(&b, 0, 0, &rb, &rows, &slices, &base, &th, &td, &bpb) == 0 && rb == 512 && rows == 1);
    puts("nvn_tex_layout OK"); return 0;
}
