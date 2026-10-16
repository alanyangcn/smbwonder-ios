#pragma once
#include <stdint.h>
// Host input model for nn::hid. The iPhone is the handheld controller (NpadId 0x20); the host front end
// (host_input.m: on-screen touch controls + GameController pads) publishes buttons/sticks with wonder_hid_publish.
enum { WONDER_NPAD_HANDHELD = 0x20 };
enum { WONDER_STYLE_FULLKEY = 1u << 0, WONDER_STYLE_HANDHELD = 1u << 1, WONDER_STYLE_JOYDUAL = 1u << 2,
       WONDER_STYLE_JOYLEFT = 1u << 3, WONDER_STYLE_JOYRIGHT = 1u << 4 };
// nn::hid::Npad*State (FullKey/Handheld/JoyDual/JoyLeft/JoyRight share this 0x28-byte layout).
typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, reserved; } wonder_npad_state;
_Static_assert(sizeof(wonder_npad_state) == 0x28, "NpadState layout");
// nn::hid::NpadButton bits. The stick-direction bits (16..23) are derived from the stick values by wonder_hid_publish.
enum {
    WONDER_BTN_A = 1u << 0, WONDER_BTN_B = 1u << 1, WONDER_BTN_X = 1u << 2, WONDER_BTN_Y = 1u << 3,
    WONDER_BTN_STICK_L = 1u << 4, WONDER_BTN_STICK_R = 1u << 5, WONDER_BTN_L = 1u << 6, WONDER_BTN_R = 1u << 7,
    WONDER_BTN_ZL = 1u << 8, WONDER_BTN_ZR = 1u << 9, WONDER_BTN_PLUS = 1u << 10, WONDER_BTN_MINUS = 1u << 11,
    WONDER_BTN_LEFT = 1u << 12, WONDER_BTN_UP = 1u << 13, WONDER_BTN_RIGHT = 1u << 14, WONDER_BTN_DOWN = 1u << 15,
    WONDER_BTN_STICK_DIRECTIONS = 0xffu << 16,   // StickL Left/Up/Right/Down 16..19, StickR 20..23
};
#define WONDER_STICK_MAX 32767
// Stick-direction bits for the given sticks (Switch y axis: up is positive). Threshold: half deflection, as the
// open-source Switch emulators use; the console firmware's exact value is not known here.
uint64_t wonder_hid_stick_bits(int32_t lx, int32_t ly, int32_t rx, int32_t ry);
void wonder_hid_reset(void);
int wonder_hid_set_supported_ids(const uint32_t *ids, uint64_t count);   // 0 ok, -1 invalid id
void wonder_hid_set_supported_styles(uint32_t styles);
uint32_t wonder_hid_style_set(uint32_t npad_id);                          // style bits currently connected
int wonder_hid_get_states(wonder_npad_state *out, int count, uint32_t npad_id, uint32_t style);  // entries written
void wonder_hid_publish(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);   // sticks clamped to ±32767
int wonder_hid_valid_npad_id(uint32_t id);
