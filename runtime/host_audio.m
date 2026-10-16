// iOS audio output for the renderer's device sink (audio_sink.h): a RemoteIO unit at 48 kHz, interleaved float stereo,
// pulling from the ring the DSP thread fills every 5 ms frame. The session is SoloAmbient (the ring/silent switch mutes,
// other apps' audio stops), as a game would use. Until the unit runs, frames are dropped; an empty ring plays silence.
#import <AVFAudio/AVFAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#include "audio_sink.h"
#include <stdatomic.h>
#include <stdio.h>
static struct wonder_audio_ring ring;
static AudioUnit unit;
static _Atomic int state;   // 0 idle, 1 starting, 2 running, -1 failed
static _Atomic uint64_t underruns, callbacks;
static OSStatus render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)ref; (void)ts; (void)bus;
    float *out = io->mBuffers[0].mData;
    uint32_t got = wonder_audio_ring_pull(&ring, out, frames);
    if (got < frames) { memset(out + 2 * got, 0, (frames - got) * 2 * sizeof(float)); if (got == 0) *flags |= kAudioUnitRenderAction_OutputIsSilence; atomic_fetch_add(&underruns, 1); }
    atomic_fetch_add(&callbacks, 1);
    return noErr;
}
static void start_unit(void) {
    NSError *error = nil; AVAudioSession *session = AVAudioSession.sharedInstance;
    if (![session setCategory:AVAudioSessionCategorySoloAmbient error:&error] || ![session setPreferredSampleRate:WONDER_AUDIO_RATE error:&error] ||
        ![session setActive:YES error:&error]) {
        fprintf(stderr, "HOST_AUDIO_OUTPUT_FAILED session: %s\n", error.localizedDescription.UTF8String); atomic_store(&state, -1); return;
    }
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    AudioStreamBasicDescription f = {WONDER_AUDIO_RATE, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0};
    AURenderCallbackStruct cb = {render, NULL};
    OSStatus s = c ? AudioComponentInstanceNew(c, &unit) : -1;
    if (!s) s = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    if (!s) s = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (!s) s = AudioUnitInitialize(unit);
    if (!s) s = AudioOutputUnitStart(unit);
    if (s) { fprintf(stderr, "HOST_AUDIO_OUTPUT_FAILED RemoteIO status %d\n", (int)s); atomic_store(&state, -1); return; }
    fprintf(stderr, "HOST_AUDIO_OUTPUT started RemoteIO %d Hz stereo float (session rate %.0f, io buffer %.1f ms)\n", WONDER_AUDIO_RATE, session.sampleRate, session.IOBufferDuration * 1000);
    atomic_store(&state, 2);
}
// Called on the DSP thread after each frame's command list (audio_platform.c). Never blocks.
void wonder_host_audio_push(const float *stereo, int frames) {
    int st = atomic_load(&state);
    if (st == 0 && atomic_compare_exchange_strong(&state, &st, 1)) {
        if (wonder_audio_ring_init(&ring, WONDER_AUDIO_RATE / 4)) { atomic_store(&state, -1); return; }   // 250 ms
        dispatch_async(dispatch_get_main_queue(), ^{ start_unit(); });
        return;
    }
    if (atomic_load(&state) != 2) return;
    wonder_audio_ring_push(&ring, stereo, (uint32_t)frames);
}
void wonder_host_audio_stats(uint64_t *under, uint64_t *calls, uint32_t *fill) { *under = atomic_load(&underruns); *calls = atomic_load(&callbacks); *fill = ring.frames ? wonder_audio_ring_fill(&ring) : 0; }
