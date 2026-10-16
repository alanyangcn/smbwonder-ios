#include "audio_sink.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
// A command list as CommandListProcessor::Setup reads it: header, a Mix command (ignored), one device sink.
static uint8_t list[0x20 + 0x30 + 0x148];
static int32_t mix[4][240];
static void build(uint32_t rate, int32_t channels, int16_t in0, int16_t in1, uint64_t upsampler, int enabled) {
    memset(list, 0, sizeof list);
    uint32_t count = 2, buffers = 4, samples = 240; uint64_t m = (uintptr_t)mix;
    memcpy(list + 4, &count, 4); memcpy(list + 8, &m, 8); memcpy(list + 0x10, &buffers, 4); memcpy(list + 0x14, &samples, 4); memcpy(list + 0x18, &rate, 4);
    uint8_t *c = list + 0x20; uint32_t magic = 0xCAFEBABEu; int16_t size = 0x30;
    memcpy(c, &magic, 4); c[4] = 1; c[5] = 0x0a; memcpy(c + 6, &size, 2);
    c += 0x30; size = 0x148; memcpy(c, &magic, 4); c[4] = (uint8_t)enabled; c[5] = 0x13; memcpy(c + 6, &size, 2);
    strcpy((char *)c + 0x20, "MainAudioOut"); memcpy(c + 0x128, &upsampler, 8); memcpy(c + 0x130, &channels, 4);
    memcpy(c + 0x134, &in0, 2); memcpy(c + 0x136, &in1, 2);
}
int main(void) {
    for (int f = 0; f < 240; f++) { mix[2][f] = f * 10; mix[3][f] = -f * 10; }
    mix[2][239] = 100000; mix[3][239] = -100000;   // clamped to int16
    float out[2 * 480]; struct wonder_audio_sink_info info; char why[160];
    build(48000, 2, 2, 3, 0, 1);
    assert(wonder_audio_sink_extract(list, sizeof list, out, 480, &info, why, sizeof why) == 240);
    assert(info.channels == 2 && info.sinks == 1 && !strcmp(info.name, "MainAudioOut"));
    assert(out[2 * 10] == 100 / 32768.0f && out[2 * 10 + 1] == -100 / 32768.0f);
    assert(out[2 * 239] == 32767 / 32768.0f && out[2 * 239 + 1] == -1.0f);
    build(48000, 1, 3, 0, 0, 1);   // mono: duplicated
    assert(wonder_audio_sink_extract(list, sizeof list, out, 480, &info, why, sizeof why) == 240 && out[20] == out[21] && out[20] == -100 / 32768.0f);
    build(48000, 2, 2, 3, 0, 0);   // disabled sink: nothing to output
    assert(wonder_audio_sink_extract(list, sizeof list, out, 480, &info, why, sizeof why) == 0);
    build(32000, 2, 2, 3, 0, 1); assert(wonder_audio_sink_extract(list, sizeof list, out, 480, &info, why, sizeof why) == -1 && strstr(why, "32000"));
    build(48000, 2, 2, 3, 0x1000, 1); assert(wonder_audio_sink_extract(list, sizeof list, out, 480, &info, why, sizeof why) == -1 && strstr(why, "upsampler"));
    build(48000, 6, 2, 3, 0, 1); assert(wonder_audio_sink_extract(list, sizeof list, out, 480, &info, why, sizeof why) == -1 && strstr(why, "6 channels"));
    build(48000, 2, 2, 9, 0, 1); assert(wonder_audio_sink_extract(list, sizeof list, out, 480, &info, why, sizeof why) == -2);   // input beyond the buffers
    build(48000, 2, 2, 3, 0, 1); list[0x20 + 0x30] = 0; assert(wonder_audio_sink_extract(list, sizeof list, out, 480, &info, why, sizeof why) == -2);   // bad magic
    // Ring: wraps, drops what does not fit, pulls what is there.
    struct wonder_audio_ring r; assert(!wonder_audio_ring_init(&r, 300));
    float in[2 * 240], got[2 * 400];
    for (int i = 0; i < 480; i++) in[i] = (float)i;
    assert(wonder_audio_ring_push(&r, in, 240) == 240 && wonder_audio_ring_push(&r, in, 240) == 60 && wonder_audio_ring_fill(&r) == 300);
    assert(wonder_audio_ring_pull(&r, got, 200) == 200 && got[0] == 0 && got[2 * 199 + 1] == 2 * 199 + 1);
    assert(wonder_audio_ring_push(&r, in, 240) == 200 && wonder_audio_ring_pull(&r, got, 400) == 300);
    assert(got[0] == 400 && got[2 * 40] == 0);   // frames 200..239 of the first push, then the wrapped third push
    puts("AUDIO_SINK_OK");
}
