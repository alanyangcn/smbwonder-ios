#pragma once
#include <stdint.h>
#include <stddef.h>
struct wonder_font_table { uint32_t tag; const void *bytes; size_t size; };
// Reconstruct one CoreText face as standalone SFNT, including table and whole-font checksums.
// Caller owns *output on success. Input font data is never changed.
int wonder_sfnt_build(const struct wonder_font_table *tables, unsigned count, uint8_t **output, size_t *size);
