#pragma once
// Widescreen (round205/206). The game frames its course camera (orthographic, height 15.5 x zoom) and its default
// perspective cameras with one 16:9 constant, main+0x2aeedb4 (read at main+0x2e2b48 and main+0x6a7630; the only
// 0x3fe38e3b in main). Set to the screen's aspect before nnMain, the cameras frame a wider view into the same 16:9
// render targets; the present then stretches the image to the screen width, which undoes the squeeze.
#include <stddef.h>
#include <stdint.h>
#define WONDER_WIDESCREEN_CONSTANT 0x2aeedb4u
#define WONDER_WIDESCREEN_ORIGINAL_BITS 0x3fe38e3bu        // 16/9 as the game stores it
#define WONDER_WIDESCREEN_MAX_ASPECT 2.4
// Aspect for a screen of w x h (either orientation): w/h of the landscape screen, at least 16:9 and at most 2.4.
float wonder_widescreen_aspect(double w, double h);
// Horizontal stretch of a 16:9 frame shown at this aspect (1 = none).
double wonder_widescreen_stretch(float aspect);
// Writes aspect over the 16:9 constant in main's image (size bytes from base). 0 = written; -1 = the constant is not
// the verified 16:9 value (another game version: nothing written); -2 = outside the image.
int wonder_widescreen_patch(uint8_t *main_base, size_t size, float aspect);
// The aspect the game runs at (16:9 until the host sets it, before nnMain).
void wonder_widescreen_set(float aspect);
float wonder_widescreen_get(void);
