#pragma once
// Metal 2 draw backend: translated shader library (this file) and, later, draw encoding.
// Shaders come from tools/wonder_shader_build.py: <key>.metal + <key>.refl + manifest.json (sha256 of both), bundled in
// the app at dm/shaders. A missing, edited, mis-staged or non-compiling shader stops by name with its key.
#include "shader_library.h"
_Noreturn void wonder_nvn_stop(const char *format, ...);
void wonder_host_draw_set_device(void *mtl_device);                  // id<MTLDevice> (unretained by the caller)
void wonder_host_shader_set_directory(const char *directory);        // directory holding manifest.json
void *wonder_host_shader_function(const char key[17], int stage);    // id<MTLFunction>, cached for the process
const struct wonder_shader_refl *wonder_host_shader_refl(const char key[17]);
