#pragma once
#include <stdint.h>
// Host input model for nn::hid. The iPhone is the handheld controller (NpadId 0x20); the host front end
// publishes buttons/sticks with wonder_hid_publish (neutral until touch/MFi input is wired).
enum { WONDER_NPAD_HANDHELD = 0x20 };
enum { WONDER_STYLE_FULLKEY = 1u << 0, WONDER_STYLE_HANDHELD = 1u << 1, WONDER_STYLE_JOYDUAL = 1u << 2,
       WONDER_STYLE_JOYLEFT = 1u << 3, WONDER_STYLE_JOYRIGHT = 1u << 4 };
// nn::hid::Npad*State (FullKey/Handheld/JoyDual/JoyLeft/JoyRight share this 0x28-byte layout).
typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, reserved; } wonder_npad_state;
_Static_assert(sizeof(wonder_npad_state) == 0x28, "NpadState layout");
void wonder_hid_reset(void);
int wonder_hid_set_supported_ids(const uint32_t *ids, uint64_t count);   // 0 ok, -1 invalid id
void wonder_hid_set_supported_styles(uint32_t styles);
uint32_t wonder_hid_style_set(uint32_t npad_id);                          // style bits currently connected
int wonder_hid_get_states(wonder_npad_state *out, int count, uint32_t npad_id, uint32_t style);  // entries written
void wonder_hid_publish(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
int wonder_hid_valid_npad_id(uint32_t id);
