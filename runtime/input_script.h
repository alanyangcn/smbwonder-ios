#pragma once
// Scripted input for unattended device rounds (WONDER_AUTO_INPUT). Comma-separated items:
//   A@20          press A once at t = 20 s (t counts from the first call of wonder_input_script_buttons)
//   A/3@15        press A every 3 s from t = 15 s on
//   A/3@15-90     ... until t = 90 s
//   RIGHT@150-480 hold RIGHT from t = 150 s to t = 480 s (no period: held, not pressed)
// Buttons: A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT. Each press is held for 150 ms; a timed range holds.
#include <stdint.h>
#define WONDER_INPUT_SCRIPT_MAX 16
struct wonder_input_item { uint64_t button; double at, every, until; };
struct wonder_input_script { int count; struct wonder_input_item items[WONDER_INPUT_SCRIPT_MAX]; };
// 0 = parsed; -1 = syntax error (err names the item).
int wonder_input_script_parse(const char *text, struct wonder_input_script *out, char *err, int errlen);
// Buttons held at time t seconds.
uint64_t wonder_input_script_buttons(const struct wonder_input_script *s, double t);
