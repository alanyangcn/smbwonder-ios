#include "audio_sink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CMD_MAGIC 0xCAFEBABEu
#define CMD_DEVICE_SINK 0x13
#define DEVICE_SINK_SIZE 0x148
static int fail(char *why, size_t n, int code, const char *format, ...) __attribute__((format(printf, 4, 5)));
#include <stdarg.h>
static int fail(char *why, size_t n, int code, const char *format, ...) {
    if (why && n) { va_list a; va_start(a, format); vsnprintf(why, n, format, a); va_end(a); }
    return code;
}
int wonder_audio_sink_extract(const void *list, size_t size, float *out, int max_frames, struct wonder_audio_sink_info *info, char *why, size_t n) {
    const uint8_t *base = list; memset(info, 0, sizeof *info);
    if (!base || size < 0x20) return fail(why, n, -2, "command list too small (%zu bytes)", size);
    uint32_t count, buffers, samples, rate; uint64_t mix;
    memcpy(&count, base + 4, 4); memcpy(&mix, base + 8, 8); memcpy(&buffers, base + 0x10, 4); memcpy(&samples, base + 0x14, 4); memcpy(&rate, base + 0x18, 4);
    info->samples = (int)samples; info->rate = (int)rate;
    const uint8_t *sink = NULL; size_t at = 0x20;
    for (uint32_t i = 0; i < count; i++) {
        if (at + 8 > size) return fail(why, n, -2, "command %u header beyond the list (%zu of %zu bytes)", i, at, size);
        uint32_t magic; int16_t csize; memcpy(&magic, base + at, 4); memcpy(&csize, base + at + 6, 2);
        if (magic != CMD_MAGIC || csize < 8 || at + (size_t)csize > size) return fail(why, n, -2, "command %u at %#zx: magic %#x size %d", i, at, magic, csize);
        if ((int8_t)base[at + 5] == CMD_DEVICE_SINK && base[at + 4]) {
            if (csize < DEVICE_SINK_SIZE) return fail(why, n, -2, "device sink command of %d bytes", csize);
            if (!sink) sink = base + at;
            info->sinks++;
        }
        at += (size_t)csize;
    }
    if (!sink) return 0;
    int32_t channels; memcpy(&channels, sink + 0x130, 4); memcpy(&info->sample_buffer, sink + 0x128, 8);
    memcpy(info->name, sink + 0x20, sizeof info->name - 1);
    info->channels = channels;
    for (int c = 0; c < 6; c++) memcpy(&info->inputs[c], sink + 0x134 + 2 * c, 2);
    if (info->sinks != 1) return fail(why, n, -1, "%d enabled device sinks", info->sinks);
    if (info->sample_buffer) return fail(why, n, -1, "device sink reads the upsampler output %#llx (rate %u)", (unsigned long long)info->sample_buffer, rate);
    if (rate != WONDER_AUDIO_RATE) return fail(why, n, -1, "renderer sample rate %u", rate);
    if (channels != 1 && channels != 2) return fail(why, n, -1, "device sink with %d channels", channels);
    if (!mix || !samples) return fail(why, n, -2, "mix buffers %#llx samples %u", (unsigned long long)mix, samples);
    if ((int)samples > max_frames) return fail(why, n, -1, "%u samples per frame exceed the %d-frame output", samples, max_frames);
    const int32_t *buf = (const int32_t *)(uintptr_t)mix;
    for (int c = 0; c < channels; c++)
        if (info->inputs[c] < 0 || (uint32_t)info->inputs[c] >= buffers) return fail(why, n, -2, "device sink input %d = buffer %d of %u", c, info->inputs[c], buffers);
    for (uint32_t f = 0; f < samples; f++) {
        for (int c = 0; c < 2; c++) {
            int32_t v = buf[(size_t)info->inputs[channels == 1 ? 0 : c] * samples + f];
            if (v > 0x7fff) v = 0x7fff; if (v < -0x8000) v = -0x8000;
            out[2 * f + c] = (float)v / 32768.0f;
        }
    }
    return (int)samples;
}

int wonder_audio_ring_init(struct wonder_audio_ring *r, uint32_t capacity) {
    memset(r, 0, sizeof *r); r->frames = calloc((size_t)capacity * 2, sizeof *r->frames);
    if (!r->frames) return -1;
    r->capacity = capacity; return 0;
}
uint32_t wonder_audio_ring_fill(const struct wonder_audio_ring *r) {
    return (uint32_t)(atomic_load_explicit(&r->written, memory_order_acquire) - atomic_load_explicit(&r->read, memory_order_acquire));
}
uint32_t wonder_audio_ring_push(struct wonder_audio_ring *r, const float *stereo, uint32_t frames) {
    uint64_t w = atomic_load_explicit(&r->written, memory_order_relaxed), rd = atomic_load_explicit(&r->read, memory_order_acquire);
    uint32_t space = r->capacity - (uint32_t)(w - rd), n = frames < space ? frames : space;
    for (uint32_t i = 0; i < n; i++) { uint32_t s = (uint32_t)((w + i) % r->capacity); r->frames[2 * s] = stereo[2 * i]; r->frames[2 * s + 1] = stereo[2 * i + 1]; }
    atomic_store_explicit(&r->written, w + n, memory_order_release);
    return n;
}
uint32_t wonder_audio_ring_pull(struct wonder_audio_ring *r, float *stereo, uint32_t frames) {
    uint64_t rd = atomic_load_explicit(&r->read, memory_order_relaxed), w = atomic_load_explicit(&r->written, memory_order_acquire);
    uint32_t have = (uint32_t)(w - rd), n = frames < have ? frames : have;
    for (uint32_t i = 0; i < n; i++) { uint32_t s = (uint32_t)((rd + i) % r->capacity); stereo[2 * i] = r->frames[2 * s]; stereo[2 * i + 1] = r->frames[2 * s + 1]; }
    atomic_store_explicit(&r->read, rd + n, memory_order_release);
    return n;
}
