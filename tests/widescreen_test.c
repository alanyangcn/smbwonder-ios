#include "widescreen.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    assert(fabsf(wonder_widescreen_aspect(2622, 1206) - 2622.0f / 1206.0f) < 1e-5f);   // iPhone 18 Pro landscape
    assert(fabsf(wonder_widescreen_aspect(1206, 2622) - 2622.0f / 1206.0f) < 1e-5f);   // portrait bounds: same aspect
    assert(wonder_widescreen_aspect(1024, 768) == 16.0f / 9.0f);                       // narrower than 16:9: unchanged
    assert(wonder_widescreen_aspect(3440, 1000) == (float)WONDER_WIDESCREEN_MAX_ASPECT);
    assert(wonder_widescreen_aspect(0, 0) == 16.0f / 9.0f);
    assert(fabs(wonder_widescreen_stretch(16.0f / 9.0f) - 1.0) < 1e-6);
    size_t size = WONDER_WIDESCREEN_CONSTANT + 16; uint8_t *image = calloc(1, size);
    uint32_t bits = WONDER_WIDESCREEN_ORIGINAL_BITS; memcpy(image + WONDER_WIDESCREEN_CONSTANT, &bits, 4);
    float a = 2.174f, got;
    assert(wonder_widescreen_patch(image, size, a) == 0); memcpy(&got, image + WONDER_WIDESCREEN_CONSTANT, 4); assert(got == a);
    assert(wonder_widescreen_patch(image, size, a) == 0);                                // same aspect again: fine
    assert(wonder_widescreen_patch(image, size, 2.0f) == -1);                            // something else there: refused
    memset(image + WONDER_WIDESCREEN_CONSTANT, 0, 4); assert(wonder_widescreen_patch(image, size, a) == -1);
    assert(wonder_widescreen_patch(image, 16, a) == -2);
    free(image); puts("widescreen OK"); return 0;
}
