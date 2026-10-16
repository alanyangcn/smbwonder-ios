#pragma once
// Device sink output of a processed AudioRenderer command list. The sdk's in-process DSP (CommandListProcessor::Process,
// sdk+0x222890) treats DeviceSink (type 0x13) as a no-op -- on hardware the ADSP firmware sends it to the output device --
// so the host reads the sink's input mix buffers after Process and hands them to iOS (host_audio.m).
// Layouts (sdk): list header (CommandListProcessor::Setup sdk+0x222844): +4 command count, +8 mix buffers (int32 per
// sample, int16 scale), +0x10 buffer count, +0x14 samples per buffer, +0x18 sample rate; commands from +0x20, each
// {u32 magic 0xCAFEBABE, u8 enabled, s8 type, s16 size, ...}. DeviceSink (CommandBuffer::Generate<DeviceSink> sdk+0x211f90,
// 0x148 bytes): +0x20 name, +0x120 session, +0x128 upsampler output buffer (0 = read the mix buffers), +0x130 input count,
// +0x134 s16 input buffer indices. Samples are clamped to int16 as CircularBufferSink does (sdk+0x223590).
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#define WONDER_AUDIO_RATE 48000
struct wonder_audio_sink_info { int sinks, channels, samples, rate; uint64_t sample_buffer; int16_t inputs[6]; char name[32]; };
// Writes the first enabled device sink's samples as interleaved float stereo (mono duplicated) into out (max_frames).
// Returns frames written; 0 when the list has no enabled device sink; -1 when the sink is not supported (why says which
// field), -2 when the list is malformed.
// upsampled: samples per buffer in the upsampler output (CommandListProcessor +0x24 after Process; the Upsample command,
// sdk+0x22324c, writes buffer index i at output + i * upsampled samples and stores that count there).
int wonder_audio_sink_extract(const void *list, size_t size, int upsampled, float *out, int max_frames, struct wonder_audio_sink_info *info, char *why, size_t n);

// Single-producer / single-consumer ring of interleaved stereo float frames (DSP thread -> iOS render callback).
struct wonder_audio_ring { float *frames; uint32_t capacity; _Atomic uint64_t written, read; };
int wonder_audio_ring_init(struct wonder_audio_ring *r, uint32_t capacity_frames);   // 0 ok
// Push drops what does not fit (returns frames stored); pull returns frames copied (the caller zero-fills the rest).
uint32_t wonder_audio_ring_push(struct wonder_audio_ring *r, const float *stereo, uint32_t frames);
uint32_t wonder_audio_ring_pull(struct wonder_audio_ring *r, float *stereo, uint32_t frames);
uint32_t wonder_audio_ring_fill(const struct wonder_audio_ring *r);
