#include "hid_state.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
    wonder_hid_reset();
    wonder_npad_state s[20];
    assert(wonder_hid_style_set(0x20) == 0);                                  // nothing supported yet
    uint32_t bad[] = {0x30}; assert(wonder_hid_set_supported_ids(bad, 1) == -1);
    uint32_t ids[] = {0, 1, 0x20}; assert(!wonder_hid_set_supported_ids(ids, 3));
    wonder_hid_set_supported_styles(WONDER_STYLE_FULLKEY);
    assert(wonder_hid_style_set(0x20) == 0);                                  // handheld style not supported by game
    wonder_hid_set_supported_styles(WONDER_STYLE_FULLKEY | WONDER_STYLE_HANDHELD);
    assert(wonder_hid_style_set(0x20) == WONDER_STYLE_HANDHELD && wonder_hid_style_set(0) == 0);
    assert(wonder_hid_get_states(s, 20, 0x20, WONDER_STYLE_HANDHELD) == 16); // NpadStateCountMax
    assert(s[0].attributes == 0x3f && s[0].buttons == 0 && s[0].sampling == s[1].sampling + 1);
    assert(wonder_hid_get_states(s, 1, 0, WONDER_STYLE_FULLKEY) == 1 && s[0].attributes == 0);   // No1: not connected
    assert(wonder_hid_get_states(s, 1, 0x20, WONDER_STYLE_FULLKEY) == 1 && s[0].attributes == 0); // wrong style view
    int64_t first; wonder_hid_get_states(s, 1, 0x20, WONDER_STYLE_HANDHELD); first = s[0].sampling;
    usleep(20000); wonder_hid_get_states(s, 1, 0x20, WONDER_STYLE_HANDHELD); assert(s[0].sampling >= first + 3);   // 5 ms period
    wonder_hid_publish(1u << 0, 100, -200, 0, 32767);                          // A held, sticks moved
    wonder_hid_get_states(s, 2, 0x20, WONDER_STYLE_HANDHELD);
    assert(s[0].buttons == 1 && s[0].lx == 100 && s[0].ly == -200 && s[0].ry == 32767);
    assert(wonder_hid_get_states(s, 0, 0x20, WONDER_STYLE_HANDHELD) == 0);
    puts("HID_STATE_OK");
}
