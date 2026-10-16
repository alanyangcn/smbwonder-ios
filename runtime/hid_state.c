// nn::hid host state. Sampling numbers advance with time at the console's 5 ms npad period; every history
// entry returned carries the current host snapshot (inputs change only when the front end publishes).
#include "hid_state.h"
#include <pthread.h>
#include <string.h>
#include <time.h>
#define ATTR_HANDHELD 0x3fu   // IsConnected | IsWired | IsLeft/RightConnected | IsLeft/RightWired
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t supported_ids[10]; static uint64_t supported_count; static uint32_t supported_styles;
static struct { uint64_t buttons; int32_t lx, ly, rx, ry; } input;
static int64_t start_ns;
static int64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
void wonder_hid_reset(void) {
    pthread_mutex_lock(&lock);
    supported_count = 0; supported_styles = 0; memset(&input, 0, sizeof input); start_ns = now_ns();
    pthread_mutex_unlock(&lock);
}
int wonder_hid_valid_npad_id(uint32_t id) { return id <= 7 || id == 0x10 || id == 0x20; }   // No1-No8, Other, Handheld
int wonder_hid_set_supported_ids(const uint32_t *ids, uint64_t count) {
    if (count > 10 || (count && !ids)) return -1;
    for (uint64_t i = 0; i < count; i++) if (!wonder_hid_valid_npad_id(ids[i])) return -1;
    pthread_mutex_lock(&lock);
    memcpy(supported_ids, ids, count * sizeof *ids); supported_count = count;
    pthread_mutex_unlock(&lock);
    return 0;
}
void wonder_hid_set_supported_styles(uint32_t styles) { pthread_mutex_lock(&lock); supported_styles = styles; pthread_mutex_unlock(&lock); }
static int handheld_connected(void) {   // holds lock
    if (!(supported_styles & WONDER_STYLE_HANDHELD)) return 0;
    for (uint64_t i = 0; i < supported_count; i++) if (supported_ids[i] == WONDER_NPAD_HANDHELD) return 1;
    return 0;
}
uint32_t wonder_hid_style_set(uint32_t id) {
    pthread_mutex_lock(&lock);
    uint32_t s = id == WONDER_NPAD_HANDHELD && handheld_connected() ? WONDER_STYLE_HANDHELD : 0;
    pthread_mutex_unlock(&lock);
    return s;
}
int wonder_hid_get_states(wonder_npad_state *out, int count, uint32_t id, uint32_t style) {
    if (!out || count <= 0) return 0;
    if (count > 16) count = 16;   // NpadStateCountMax
    pthread_mutex_lock(&lock);
    int connected = id == WONDER_NPAD_HANDHELD && style == WONDER_STYLE_HANDHELD && handheld_connected();
    int64_t sampling = (now_ns() - start_ns) / 5000000;
    for (int i = 0; i < count; i++) {
        out[i] = (wonder_npad_state){0};
        out[i].sampling = sampling - i;   // newest first
        if (connected) { out[i].buttons = input.buttons; out[i].lx = input.lx; out[i].ly = input.ly; out[i].rx = input.rx; out[i].ry = input.ry; out[i].attributes = ATTR_HANDHELD; }
    }
    pthread_mutex_unlock(&lock);
    return count;
}
uint64_t wonder_hid_stick_bits(int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    const int32_t t = WONDER_STICK_MAX / 2; uint64_t b = 0;
    if (lx < -t) b |= 1u << 16; if (ly > t) b |= 1u << 17; if (lx > t) b |= 1u << 18; if (ly < -t) b |= 1u << 19;
    if (rx < -t) b |= 1u << 20; if (ry > t) b |= 1u << 21; if (rx > t) b |= 1u << 22; if (ry < -t) b |= 1u << 23;
    return b;
}
static int32_t clamp_axis(int32_t v) { return v > WONDER_STICK_MAX ? WONDER_STICK_MAX : v < -WONDER_STICK_MAX ? -WONDER_STICK_MAX : v; }
void wonder_hid_publish(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    lx = clamp_axis(lx); ly = clamp_axis(ly); rx = clamp_axis(rx); ry = clamp_axis(ry);
    buttons = (buttons & ~(uint64_t)WONDER_BTN_STICK_DIRECTIONS) | wonder_hid_stick_bits(lx, ly, rx, ry);
    pthread_mutex_lock(&lock); input.buttons = buttons; input.lx = lx; input.ly = ly; input.rx = rx; input.ry = ry; pthread_mutex_unlock(&lock);
}
