#pragma once
// Metal 2 draw backend: translated shader library (this file) and, later, draw encoding.
// Shaders come from tools/wonder_shader_build.py: <key>.metal + <key>.refl + manifest.json (sha256 of both), bundled in
// the app at dm/shaders. A missing, edited, mis-staged or non-compiling shader stops by name with its key.
#include "shader_library.h"
_Noreturn void wonder_nvn_stop(const char *format, ...);
void wonder_host_draw_set_device(void *mtl_device);                  // id<MTLDevice> (unretained by the caller)
void wonder_host_shader_set_directory(const char *directory);        // directory holding manifest.json
void *wonder_host_shader_function(const char key[17], int stage);    // id<MTLFunction>, cached for the process
void wonder_host_draw_forget_sampled(uint64_t serial);              // drops the CPU-uploaded copy of a finalized texture
unsigned wonder_host_draw_sampled_count(void);
const struct wonder_shader_refl *wonder_host_shader_refl(const char key[17]);
#ifdef __OBJC__
#import <Metal/Metal.h>
#include "nvn_tex.h"
// Render target descriptor for an NVN texture: 2D / rectangle, 2D array, cube and cube array (depth = layer-faces,
// 6 per cube; NVN layer-face index = Metal slice). nil for targets the host does not render to.
MTLTextureDescriptor *wonder_host_render_target_descriptor(const nvn_texdesc *d, MTLPixelFormat format);
// Metal slice range of an NVN view's layers (min_layer, layers; layers 0 = the rest). Cube and cube-array views must
// cover whole cubes: a cube-aligned range is rounded up to whole cubes. Length 0 = not representable.
NSRange wonder_host_view_slices(MTLTextureType type, NSUInteger array_length, NSUInteger min_layer, NSUInteger layers);
#endif
