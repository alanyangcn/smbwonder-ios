// NVN state objects (value types the game copies freely): packed in place within the NVN reserved sizes.
// Layouts follow D2R dm/gfx/nvn/nvn_obj.h; defaults follow NVN/GL. Enum ranges are validated (stop on misuse).
// NVNboolean parameters arrive in w0..; only the low 8 bits are meaningful (Switch callers do not extend).
#include "nvn_device.h"
#include "nvn_state.h"
#include <string.h>
typedef wonder_nvn_blend_state blend_st; typedef wonder_nvn_color_state color_st; typedef wonder_nvn_channel_mask_state chmask_st;
typedef wonder_nvn_multisample_state ms_st; typedef wonder_nvn_polygon_state poly_st; typedef wonder_nvn_depth_stencil_state ds_st;
typedef wonder_nvn_vertex_attrib_state va_st; typedef wonder_nvn_vertex_stream_state vs_st;
#define B(x) (((unsigned)(x) & 0xff) != 0)
static void need(const void *p, const char *who) { if (!p) wonder_nvn_stop("NVN_INVALID_OBJECT %s state=NULL", who); }
static int range(int v, int lo, int hi, const char *who) { if (v < lo || v > hi) wonder_nvn_stop("NVN_INVALID_ARGUMENT %s value=%d", who, v); return v; }

// Blend: funcs 1..0x13, equations 1..5, target 0..7
void wonder_nvnBlendStateSetDefaults(void *s) { need(s, "nvnBlendStateSetDefaults"); *(blend_st *)s = (blend_st){0, 2, 1, 2, 1, 1, 1}; }
void wonder_nvnBlendStateSetBlendTarget(void *s, int t) { need(s, __func__); ((blend_st *)s)->target = (unsigned)range(t, 0, 7, "nvnBlendStateSetBlendTarget"); }
static int blend_func(int v, const char *who) { return v >= 0x61 && v <= 0x64 ? v : range(v, 1, 0x13, who); }
void wonder_nvnBlendStateSetBlendFunc(void *s, int sr, int dr, int sa, int da) {
    need(s, __func__); blend_st *o = s; const char *w = "nvnBlendStateSetBlendFunc";
    // NVNblendFunc: ZERO..ONE_MINUS_SRC1_ALPHA (1..0x13) and CONSTANT_COLOR..ONE_MINUS_CONSTANT_ALPHA (0x61..0x64).
    o->src_rgb = (unsigned)blend_func(sr, w); o->dst_rgb = (unsigned)blend_func(dr, w); o->src_a = (unsigned)blend_func(sa, w); o->dst_a = (unsigned)blend_func(da, w);
}
void wonder_nvnBlendStateSetBlendEquation(void *s, int er, int ea) {
    need(s, __func__); ((blend_st *)s)->eq_rgb = (unsigned)range(er, 1, 5, "nvnBlendStateSetBlendEquation"); ((blend_st *)s)->eq_a = (unsigned)range(ea, 1, 5, "nvnBlendStateSetBlendEquation");
}
// Color: blend enable per target, logic op 0..15, alpha test func 1..8 (+ reference, kept as half-precision bits)
void wonder_nvnColorStateSetDefaults(void *s) { need(s, "nvnColorStateSetDefaults"); *(color_st *)s = (color_st){0, 3, 8, 0}; }
void wonder_nvnColorStateSetBlendEnable(void *s, int i, unsigned en) {
    need(s, __func__); color_st *o = s; range(i, 0, 7, "nvnColorStateSetBlendEnable");
    o->blend_en = B(en) ? (o->blend_en | 1u << i) : (o->blend_en & ~(1u << i));
}
void wonder_nvnColorStateSetLogicOp(void *s, int op) { need(s, __func__); ((color_st *)s)->logic_op = (unsigned)range(op, 0, 15, "nvnColorStateSetLogicOp"); }
void wonder_nvnColorStateSetAlphaTest(void *s, int func) { need(s, __func__); ((color_st *)s)->alpha_func = (unsigned)range(func, 1, 8, "nvnColorStateSetAlphaTest"); }
// Channel mask: 4 bits per target (r=1 g=2 b=4 a=8), set = write
void wonder_nvnChannelMaskStateSetDefaults(void *s) { need(s, "nvnChannelMaskStateSetDefaults"); ((chmask_st *)s)->mask = 0xffffffffu; }
void wonder_nvnChannelMaskStateSetChannelMask(void *s, int i, unsigned r, unsigned g, unsigned b, unsigned a) {
    need(s, __func__); range(i, 0, 7, "nvnChannelMaskStateSetChannelMask");
    uint32_t m = (B(r) ? 1u : 0) | (B(g) ? 2u : 0) | (B(b) ? 4u : 0) | (B(a) ? 8u : 0);
    chmask_st *o = s; o->mask = (o->mask & ~(0xfu << (4 * i))) | m << (4 * i);
}
// Multisample
void wonder_nvnMultisampleStateSetDefaults(void *s) { need(s, "nvnMultisampleStateSetDefaults"); memset(s, 0, sizeof(ms_st)); ((ms_st *)s)->enable = 1; }
void wonder_nvnMultisampleStateSetMultisampleEnable(void *s, unsigned e) { need(s, __func__); ((ms_st *)s)->enable = B(e); }
void wonder_nvnMultisampleStateSetSamples(void *s, int n) {
    need(s, __func__); if (n != 0 && n != 1 && n != 2 && n != 4 && n != 8) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnMultisampleStateSetSamples %d", n);
    ((ms_st *)s)->samples = (uint8_t)n;
}
void wonder_nvnMultisampleStateSetAlphaToCoverageEnable(void *s, unsigned e) { need(s, __func__); ((ms_st *)s)->a2c = B(e); }
void wonder_nvnMultisampleStateSetAlphaToCoverageDither(void *s, unsigned e) { need(s, __func__); ((ms_st *)s)->a2c_dither = B(e); }
// Polygon: cull 0..3, front face 0..1, mode 0..2, offset enables bitmask 0..7
void wonder_nvnPolygonStateSetDefaults(void *s) { need(s, "nvnPolygonStateSetDefaults"); *(poly_st *)s = (poly_st){0, 1, 2, 0}; }
void wonder_nvnPolygonStateSetCullFace(void *s, int f) { need(s, __func__); ((poly_st *)s)->cull = (unsigned)range(f, 0, 3, "nvnPolygonStateSetCullFace"); }
void wonder_nvnPolygonStateSetFrontFace(void *s, int f) { need(s, __func__); ((poly_st *)s)->front = (unsigned)range(f, 0, 1, "nvnPolygonStateSetFrontFace"); }
void wonder_nvnPolygonStateSetPolygonMode(void *s, int m) { need(s, __func__); ((poly_st *)s)->mode = (unsigned)range(m, 0, 2, "nvnPolygonStateSetPolygonMode"); }
void wonder_nvnPolygonStateSetPolygonOffsetEnables(void *s, int e) { need(s, __func__); ((poly_st *)s)->offset_en = (unsigned)range(e, 0, 7, "nvnPolygonStateSetPolygonOffsetEnables"); }
// Depth/stencil: depth func 1..8, stencil func 1..8, stencil op 1..8
void wonder_nvnDepthStencilStateSetDefaults(void *s) { need(s, "nvnDepthStencilStateSetDefaults"); *(ds_st *)s = (ds_st){0, 1, 2, 0, 8, 8, 1, 1, 1, 1, 1, 1}; }
void wonder_nvnDepthStencilStateSetDepthTestEnable(void *s, unsigned e) { need(s, __func__); ((ds_st *)s)->dt = B(e); }
void wonder_nvnDepthStencilStateSetDepthWriteEnable(void *s, unsigned e) { need(s, __func__); ((ds_st *)s)->dw = B(e); }
void wonder_nvnDepthStencilStateSetDepthFunc(void *s, int f) { need(s, __func__); ((ds_st *)s)->df = (unsigned)range(f, 1, 8, "nvnDepthStencilStateSetDepthFunc"); }
void wonder_nvnDepthStencilStateSetStencilTestEnable(void *s, unsigned e) { need(s, __func__); ((ds_st *)s)->st = B(e); }
void wonder_nvnDepthStencilStateSetStencilFunc(void *s, int face, int fn) {
    need(s, __func__); ds_st *o = s; range(face, 1, 3, "nvnDepthStencilStateSetStencilFunc face"); range(fn, 1, 8, "nvnDepthStencilStateSetStencilFunc");
    if (face & 1) o->sf_f = (unsigned)fn;
    if (face & 2) o->sf_b = (unsigned)fn;
}
void wonder_nvnDepthStencilStateSetStencilOp(void *s, int face, int fail, int zfail, int zpass) {
    need(s, __func__); ds_st *o = s; const char *w = "nvnDepthStencilStateSetStencilOp"; range(face, 1, 3, w);
    range(fail, 1, 8, w); range(zfail, 1, 8, w); range(zpass, 1, 8, w);
    if (face & 1) { o->fail_f = (unsigned)fail; o->zfail_f = (unsigned)zfail; o->zpass_f = (unsigned)zpass; }
    if (face & 2) { o->fail_b = (unsigned)fail; o->zfail_b = (unsigned)zfail; o->zpass_b = (unsigned)zpass; }
}
// Vertex attribute / stream
void wonder_nvnVertexAttribStateSetDefaults(void *s) { need(s, "nvnVertexAttribStateSetDefaults"); *(va_st *)s = (va_st){0, 0, 0}; }
void wonder_nvnVertexAttribStateSetFormat(void *s, int format, ptrdiff_t offset) {
    need(s, __func__); range(format, 0, 0xff, "nvnVertexAttribStateSetFormat");
    if (offset < 0 || offset >= (1 << 19)) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnVertexAttribStateSetFormat offset=%td", offset);
    ((va_st *)s)->fmt = (unsigned)format; ((va_st *)s)->offset = (uint32_t)offset;
}
void wonder_nvnVertexAttribStateSetStreamIndex(void *s, int i) { need(s, __func__); ((va_st *)s)->stream = (unsigned)range(i, 0, 15, "nvnVertexAttribStateSetStreamIndex"); }
void wonder_nvnVertexStreamStateSetDefaults(void *s) { need(s, "nvnVertexStreamStateSetDefaults"); *(vs_st *)s = (vs_st){0, 0}; }
void wonder_nvnVertexStreamStateSetStride(void *s, ptrdiff_t stride) {
    need(s, __func__); if (stride < 0 || stride > 0xffff) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnVertexStreamStateSetStride %td", stride);
    ((vs_st *)s)->stride = (uint32_t)stride;
}
void wonder_nvnVertexStreamStateSetDivisor(void *s, int d) { need(s, __func__); ((vs_st *)s)->divisor = (uint32_t)range(d, 0, 0x7fffffff, "nvnVertexStreamStateSetDivisor"); }

const struct wonder_nvn_entry wonder_nvn_state_entries[] = {
#define E(n) {#n, (void *)wonder_##n}
    E(nvnBlendStateSetDefaults), E(nvnBlendStateSetBlendTarget), E(nvnBlendStateSetBlendFunc), E(nvnBlendStateSetBlendEquation),
    E(nvnColorStateSetDefaults), E(nvnColorStateSetBlendEnable), E(nvnColorStateSetLogicOp), E(nvnColorStateSetAlphaTest),
    E(nvnChannelMaskStateSetDefaults), E(nvnChannelMaskStateSetChannelMask),
    E(nvnMultisampleStateSetDefaults), E(nvnMultisampleStateSetMultisampleEnable), E(nvnMultisampleStateSetSamples),
    E(nvnMultisampleStateSetAlphaToCoverageEnable), E(nvnMultisampleStateSetAlphaToCoverageDither),
    E(nvnPolygonStateSetDefaults), E(nvnPolygonStateSetCullFace), E(nvnPolygonStateSetFrontFace), E(nvnPolygonStateSetPolygonMode),
    E(nvnPolygonStateSetPolygonOffsetEnables),
    E(nvnDepthStencilStateSetDefaults), E(nvnDepthStencilStateSetDepthTestEnable), E(nvnDepthStencilStateSetDepthWriteEnable),
    E(nvnDepthStencilStateSetDepthFunc), E(nvnDepthStencilStateSetStencilTestEnable), E(nvnDepthStencilStateSetStencilFunc),
    E(nvnDepthStencilStateSetStencilOp),
    E(nvnVertexAttribStateSetDefaults), E(nvnVertexAttribStateSetFormat), E(nvnVertexAttribStateSetStreamIndex),
    E(nvnVertexStreamStateSetDefaults), E(nvnVertexStreamStateSetStride), E(nvnVertexStreamStateSetDivisor),
#undef E
};
const unsigned wonder_nvn_state_count = sizeof wonder_nvn_state_entries / sizeof *wonder_nvn_state_entries;
