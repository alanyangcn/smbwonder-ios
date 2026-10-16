#include "input_script.h"
#include "hid_state.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HOLD 0.15
static const struct { const char *name; uint64_t bit; } names[] = {
    {"A", WONDER_BTN_A}, {"B", WONDER_BTN_B}, {"X", WONDER_BTN_X}, {"Y", WONDER_BTN_Y}, {"L", WONDER_BTN_L}, {"R", WONDER_BTN_R},
    {"ZL", WONDER_BTN_ZL}, {"ZR", WONDER_BTN_ZR}, {"PLUS", WONDER_BTN_PLUS}, {"MINUS", WONDER_BTN_MINUS},
    {"UP", WONDER_BTN_UP}, {"DOWN", WONDER_BTN_DOWN}, {"LEFT", WONDER_BTN_LEFT}, {"RIGHT", WONDER_BTN_RIGHT},
};
int wonder_input_script_parse(const char *text, struct wonder_input_script *out, char *err, int errlen) {
    memset(out, 0, sizeof *out);
    if (!text) return 0;
    char buf[512]; snprintf(buf, sizeof buf, "%s", text);
    for (char *save = NULL, *item = strtok_r(buf, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
        while (*item == ' ') item++;
        if (!*item) continue;
        if (out->count == WONDER_INPUT_SCRIPT_MAX) { snprintf(err, errlen, "more than %d items", WONDER_INPUT_SCRIPT_MAX); return -1; }
        char name[8]; size_t n = strcspn(item, "/@");
        if (n == 0 || n >= sizeof name) { snprintf(err, errlen, "bad button in '%s'", item); return -1; }
        memcpy(name, item, n); name[n] = 0;
        struct wonder_input_item it = {0, 0, 0, INFINITY};
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (!strcmp(names[i].name, name)) it.button = names[i].bit;
        if (!it.button) { snprintf(err, errlen, "unknown button '%s'", name); return -1; }
        const char *p = item + n; char *end;
        if (*p == '/') { it.every = strtod(p + 1, &end); if (end == p + 1 || it.every <= HOLD) { snprintf(err, errlen, "bad period in '%s'", item); return -1; } p = end; }
        if (*p != '@') { snprintf(err, errlen, "missing @time in '%s'", item); return -1; }
        it.at = strtod(p + 1, &end); if (end == p + 1 || it.at < 0) { snprintf(err, errlen, "bad time in '%s'", item); return -1; } p = end;
        if (*p == '-') { it.until = strtod(p + 1, &end); if (end == p + 1 || it.until < it.at) { snprintf(err, errlen, "bad end in '%s'", item); return -1; } p = end; }
        if (*p) { snprintf(err, errlen, "trailing '%s'", p); return -1; }
        out->items[out->count++] = it;
    }
    return 0;
}
uint64_t wonder_input_script_buttons(const struct wonder_input_script *s, double t) {
    uint64_t b = 0;
    for (int i = 0; i < s->count; i++) {
        const struct wonder_input_item *it = &s->items[i];
        if (t < it->at || t > it->until) continue;
        double phase = it->every > 0 ? fmod(t - it->at, it->every) : t - it->at;
        if (phase < HOLD) b |= it->button;
    }
    return b;
}
