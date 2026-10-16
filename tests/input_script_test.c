#include "input_script.h"
#include "hid_state.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    struct wonder_input_script s; char err[96];
    assert(!wonder_input_script_parse("A/3@15-90, PLUS@5", &s, err, sizeof err) && s.count == 2);
    assert(wonder_input_script_buttons(&s, 5.05) == WONDER_BTN_PLUS && wonder_input_script_buttons(&s, 5.2) == 0);   // 150 ms hold
    assert(wonder_input_script_buttons(&s, 14.9) == 0 && wonder_input_script_buttons(&s, 15.1) == WONDER_BTN_A);
    assert(wonder_input_script_buttons(&s, 16.0) == 0 && wonder_input_script_buttons(&s, 18.05) == WONDER_BTN_A);    // every 3 s
    assert(wonder_input_script_buttons(&s, 90.5) == 0);                                                                // until 90 s
    assert(!wonder_input_script_parse("", &s, err, sizeof err) && s.count == 0);
    assert(wonder_input_script_parse("Q@1", &s, err, sizeof err) == -1);
    assert(wonder_input_script_parse("A/3", &s, err, sizeof err) == -1);       // no start time
    assert(wonder_input_script_parse("A/0.1@1", &s, err, sizeof err) == -1);   // period shorter than the hold
    assert(wonder_input_script_parse("A@9-3", &s, err, sizeof err) == -1);
    assert(!wonder_input_script_parse("RIGHT@10-20,A/1@10-20", &s, err, sizeof err));   // hold right, jump every second
    assert(wonder_input_script_buttons(&s, 9.9) == 0 && wonder_input_script_buttons(&s, 15.5) == WONDER_BTN_RIGHT);
    assert(wonder_input_script_buttons(&s, 15.05) == (WONDER_BTN_RIGHT | WONDER_BTN_A) && wonder_input_script_buttons(&s, 20.1) == 0);
    puts("INPUT_SCRIPT_OK");
}
