#include "widescreen.h"
#include <string.h>
float wonder_widescreen_aspect(double w, double h) {
    if (w <= 0 || h <= 0) return 16.0f / 9.0f;
    double a = w > h ? w / h : h / w;
    if (a < 16.0 / 9.0) a = 16.0 / 9.0;
    if (a > WONDER_WIDESCREEN_MAX_ASPECT) a = WONDER_WIDESCREEN_MAX_ASPECT;
    return (float)a;
}
double wonder_widescreen_stretch(float aspect) { return aspect / (16.0 / 9.0); }
int wonder_widescreen_patch(uint8_t *main_base, size_t size, float aspect) {
    if (!main_base || size < WONDER_WIDESCREEN_CONSTANT + 4) return -2;
    uint32_t bits; memcpy(&bits, main_base + WONDER_WIDESCREEN_CONSTANT, 4);
    if (bits != WONDER_WIDESCREEN_ORIGINAL_BITS) {
        float f; memcpy(&f, &bits, 4);
        if (f != aspect) return -1;   // already patched to this aspect is fine; anything else is not the verified build
        return 0;
    }
    memcpy(main_base + WONDER_WIDESCREEN_CONSTANT, &aspect, 4);
    return 0;
}
static float current = 16.0f / 9.0f;
void wonder_widescreen_set(float aspect) { current = aspect; }
float wonder_widescreen_get(void) { return current; }
