// nn::audio platform layer for Wonder. The sdk module links the complete AudioRenderer stack: client
// (nn::audio::*), in-process server (nn::audio::server::AudioRendererManagerImpl, AudioRenderSystem,
// InfoUpdater, CommandGenerator) and the DSP command processor (nn::audio::dsp::CommandListProcessor).
// Only the platform below it is a stub in this build. The host therefore:
//  - routes CreateAudioRendererManagerByHipc to the sdk's own CreateAudioRendererManagerByDfc (the
//    manual-execution path the SDK already uses when executionMode == 1, sdk+0x1e0a38), so update
//    parsing, voice/wave-buffer state and command generation are the SDK's code, not a host model;
//  - supplies the DSP: dsp::SendCommandBuffer runs the sdk's CommandListProcessor exactly as
//    AudioRendererImpl::ExecuteAudioRendererRendering does (sdk+0x20ccd4..0x20ccf4), and dsp::Wait
//    paces AudioRenderSystemManager::ThreadFunction to the 5 ms DSP frame on the host clock;
//  - answers the audio-device queries for an iPhone speaker (handheld built-in speaker, stereo).
// Output: the final mix stays in renderer memory (silent sink with real-time pacing). Routing the device
// sink to iOS audio output is the next audio step.
#include "hle.h"
#include "kernel_events.h"
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <string.h>
#include <pthread.h>

#define DSP_FRAME_NS 5000000ull      // TX1 ADSP renders at 200 Hz (240 samples @ 48 kHz, 160 @ 32 kHz)
static void *required(const char *name) { void *p = guest_sym(name); if (!p) die("audio missing SDK entry %s", name); return p; }

// ---------- Renderer manager: in-process (DFC) server ----------
static void *by_dfc;   // nn::audio::CreateAudioRendererManagerByDfc(): SharedPointer returned through x8
void *wonder_audio_resolve_dfc(void);
void *wonder_audio_resolve_dfc(void) {
    if (!by_dfc) by_dfc = required("_ZN2nn5audio31CreateAudioRendererManagerByDfcEv");
    static _Atomic int logged;
    if (!atomic_exchange(&logged, 1)) LOG("HOST_AUDIO_RENDERER_MANAGER in-process (DFC) server at %p", by_dfc);
    return by_dfc;
}
// The import returns a C++ SharedPointer indirectly through x8, which a C wrapper cannot forward, so the
// forced entry is a trampoline: resolve the sdk entry (no arguments to preserve except x8), then tail-call it.
__attribute__((naked)) static void dfc_trampoline(void) {
    __asm__ volatile(
        "stp x29, x30, [sp, #-32]!\n"
        "mov x29, sp\n"
        "str x8, [sp, #16]\n"
        "bl _wonder_audio_resolve_dfc\n"
        "mov x16, x0\n"
        "ldr x8, [sp, #16]\n"
        "ldp x29, x30, [sp], #32\n"
        "br x16\n");
}
__attribute__((used, section("__DATA,swforce"))) static const hle_force_ent hle_force_audio_manager = {
    "_ZN2nn5audio32CreateAudioRendererManagerByHipcEv", (void *)dfc_trampoline,
    "Use the sdk's in-process AudioRenderer server (DFC) instead of the audren:u service"};

static void signal_active_renderers(void);
// ---------- DSP ----------
typedef void (*clp_set_time)(void *, unsigned);
typedef void (*clp_setup)(void *, const void *, unsigned long);
typedef void (*clp_process)(void *);
static clp_set_time clp_set_time_max; static clp_setup clp_setup_fn; static clp_process clp_process_fn;
static _Atomic uint64_t dsp_frames, dsp_lists;
static uint64_t next_deadline;   // mach absolute time of the next DSP frame
static mach_timebase_info_data_t timebase;
static uint64_t ns_to_abs(uint64_t ns) { return ns * timebase.denom / timebase.numer; }
static void dsp_bind(void) {
    if (clp_process_fn) return;
    mach_timebase_info(&timebase);
    clp_set_time_max = required("_ZN2nn5audio3dsp20CommandListProcessor17SetProcessTimeMaxEj");
    clp_setup_fn = required("_ZN2nn5audio3dsp20CommandListProcessor5SetupEPKvm");
    clp_process_fn = required("_ZN2nn5audio3dsp20CommandListProcessor7ProcessEv");
}
HLE_FORCE("_ZN2nn5audio3dsp5StartEv", "Host DSP: frame clock starts on the host monotonic timer", Result, h_dsp_start, void) {
    dsp_bind(); next_deadline = mach_absolute_time() + ns_to_abs(DSP_FRAME_NS);
    LOG("HOST_AUDIO_DSP_START frame_ns=%llu", DSP_FRAME_NS); return 0;
}
// AudioRenderSystemManager::ThreadFunction (sdk+0x20dc10): Wait -> SendCommandToDsp per system -> Signal.
HLE_FORCE("_ZN2nn5audio3dsp4WaitEv", "Host DSP: block until the next 5 ms DSP frame (absolute schedule, no drift)", void, h_dsp_wait, void) {
    dsp_bind();
    uint64_t now = mach_absolute_time();
    if (!next_deadline) next_deadline = now;
    if (now < next_deadline) mach_wait_until(next_deadline);
    next_deadline += ns_to_abs(DSP_FRAME_NS);
    if (mach_absolute_time() > next_deadline + ns_to_abs(20 * DSP_FRAME_NS)) next_deadline = mach_absolute_time();   // resync after a stall
    uint64_t frames = atomic_fetch_add(&dsp_frames, 1) + 1;
    if (frames == 1 || frames % 2000 == 0) LOG("HOST_AUDIO_DSP_FRAME frames=%llu lists=%llu", frames, (unsigned long long)atomic_load(&dsp_lists));
}
// sdk+0x20c8f4: SendCommandBuffer(session, command list address, size, time limit, reset, start tick).
// Addresses are CPU addresses (dsp::MapUserPointer is the identity in this in-process configuration).
HLE_FORCE("_ZN2nn5audio3dsp17SendCommandBufferEimmibm", "Host DSP: run the sdk's CommandListProcessor on the submitted list",
          void, h_dsp_send, int session, uint64_t address, uint64_t size, int time_limit, uint32_t reset, uint64_t tick) {
    dsp_bind();
    if (!address || !size) die("audio SendCommandBuffer session=%d address=%#llx size=%#llx", session, (unsigned long long)address, (unsigned long long)size);
    _Alignas(16) uint8_t processor[0x58];   // AudioRendererImpl::ExecuteAudioRendererRendering zeroes sp+8..sp+0x60
    memset(processor, 0, sizeof processor);
    clp_set_time_max(processor, 0xffffffffu);
    clp_setup_fn(processor, (const void *)address, (unsigned long)size);
    clp_process_fn(processor);
    signal_active_renderers();   // the server signals its renderer event right after sending (sdk+0x20c908)
    uint64_t lists = atomic_fetch_add(&dsp_lists, 1) + 1;
    if (lists <= 4) LOG("HOST_AUDIO_DSP_LIST session=%d address=%#llx size=%#llx time_limit=%d reset=%u tick=%llu",
                        session, (unsigned long long)address, (unsigned long long)size, time_limit, reset & 0xff, (unsigned long long)tick);
}

// ---------- Audio device (iPhone speaker presented as the handheld built-in speaker) ----------
#define DEVICE_NAME 256
static const char *const output_devices[] = {"AudioTvOutput", "AudioStereoJackOutput", "AudioBuiltInSpeakerOutput"};
static const char *const active_output = "AudioBuiltInSpeakerOutput";
// AudioUsbDeviceOutput exists on current firmware (the game sets its volume) but no USB audio device is attached,
// so it is not listed; only its volume setting is kept.
static const char *const volume_devices[] = {"AudioTvOutput", "AudioStereoJackOutput", "AudioBuiltInSpeakerOutput", "AudioUsbDeviceOutput"};
static float device_volume[4] = {1, 1, 1, 1};
static pthread_mutex_t device_lock = PTHREAD_MUTEX_INITIALIZER;
static int device_index(const char *name) {
    if (!name) die("audio device name=NULL");
    for (int i = 0; i < 4; i++) if (!strncmp(name, volume_devices[i], DEVICE_NAME)) return i;
    return -1;
}
static int list_devices(char *names, int count) {
    if (count < 0 || (count && !names)) die("audio ListAudioDeviceName count=%d names=%p", count, (void *)names);
    int n = count < 3 ? count : 3;
    for (int i = 0; i < n; i++) { memset(names + i * DEVICE_NAME, 0, DEVICE_NAME); strcpy(names + i * DEVICE_NAME, output_devices[i]); }
    return n;
}
HLE_FORCE("_ZN2nn5audio6server6detail19ListAudioDeviceNameEPNS0_15AudioDeviceNameEij", "Host audio devices: TV, headphone jack, built-in speaker",
          int, h_list_devices, char *names, int count, unsigned session) { (void)session; return list_devices(names, count); }
HLE_FORCE("_ZN2nn5audio6server6detail25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEij", "Host audio output devices: TV, headphone jack, built-in speaker",
          int, h_list_output_devices, char *names, int count, unsigned session) { (void)session; return list_devices(names, count); }
static void active_name(char *name) {
    if (!name) die("audio GetActiveAudioDeviceName name=NULL");
    memset(name, 0, DEVICE_NAME); strcpy(name, active_output);
}
HLE_FORCE("_ZN2nn5audio6server6detail24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameEj", "Active output is the built-in speaker (iPhone speaker)",
          void, h_active_device, char *name, unsigned session) { (void)session; active_name(name); }
HLE_FORCE("_ZN2nn5audio6server6detail30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameEj", "Active output is the built-in speaker (iPhone speaker)",
          void, h_active_output_device, char *name, unsigned session) { (void)session; active_name(name); }
HLE_FORCE("_ZN2nn5audio6server6detail21GetActiveChannelCountEv", "Built-in speaker output is stereo", int, h_active_channels, void) { return 2; }
HLE_FORCE("_ZN2nn5audio6server6detail26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEfRKNS_6applet20AppletResourceUserIdEj",
          "Per-device output volume kept by the host", void, h_set_device_volume, const char *name, float volume, const void *aruid, unsigned session) {
    // AudioDeviceImpl::SetAudioDeviceOutputVolumeInternal (sdk+0x2206ec) clamps to [0, 128] before calling here.
    (void)aruid; (void)session; int i = device_index(name);
    if (i < 0 || !(volume >= 0.f && volume <= 128.f)) die("audio SetAudioDeviceOutputVolume name=%.32s volume=%f", name, volume);
    pthread_mutex_lock(&device_lock); device_volume[i] = volume; pthread_mutex_unlock(&device_lock);
    LOG("HOST_AUDIO_DEVICE_VOLUME %s=%.3f", volume_devices[i], volume);
}
HLE_FORCE("_ZN2nn5audio6server6detail26GetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameERKNS_6applet20AppletResourceUserIdEj",
          "Per-device output volume kept by the host", float, h_get_device_volume, const char *name, const void *aruid, unsigned session) {
    (void)aruid; (void)session; int i = device_index(name);
    if (i < 0) die("audio GetAudioDeviceOutputVolume unknown device %.32s", name);
    pthread_mutex_lock(&device_lock); float v = device_volume[i]; pthread_mutex_unlock(&device_lock); return v;
}
// GetAudioSystemMasterVolumeSetting (sdk+0x220be0, stub returns 0.0) is called directly inside the sdk, not
// through a symbol relocation, so it cannot be taken over here; it must be handled before audio output is routed.
// Device events: real kernel events (readable handle returned); the host never changes the output device,
// so they are never signaled.
static Result device_event(unsigned *handle, const char *what) {
    if (!handle) die("audio %s handle=NULL", what);
    uint64_t r[8] = {0};
    if (!wonder_event_call(0x45, r) || r[0]) die("audio %s CreateEvent failed result=%#llx", what, (unsigned long long)r[0]);
    *handle = (unsigned)r[2];
    LOG("HOST_AUDIO_DEVICE_EVENT %s readable=%#x (never signaled: output device is fixed)", what, *handle);
    return 0;
}
HLE_FORCE("_ZN2nn5audio6server6detail27QueryAudioDeviceSystemEventEPjRKNS_6applet20AppletResourceUserIdE", "Kernel event; fixed output device never changes",
          Result, h_device_system_event, unsigned *handle, const void *aruid) { (void)aruid; return device_event(handle, "QueryAudioDeviceSystemEvent"); }
HLE_FORCE("_ZN2nn5audio6server6detail38QueryAudioDeviceInputNotificationEventEPjRKNS_6applet20AppletResourceUserIdE", "Kernel event; no input device changes",
          Result, h_device_input_event, unsigned *handle, const void *aruid) { (void)aruid; return device_event(handle, "QueryAudioDeviceInputNotificationEvent"); }
HLE_FORCE("_ZN2nn5audio6server6detail39QueryAudioDeviceOutputNotificationEventEPjRKNS_6applet20AppletResourceUserIdE", "Kernel event; fixed output device never changes",
          Result, h_device_output_event, unsigned *handle, const void *aruid) { (void)aruid; return device_event(handle, "QueryAudioDeviceOutputNotificationEvent"); }

// ---------- Renderer system event (auto execution through the in-process server) ----------
// The in-process server creates its renderer SystemEvent process-locally (sdk+0x20da24/0x20da48 pass
// interProcess=false), so AudioRenderSystem::QuerySystemEvent (sdk+0x20c978) yields no handle and the client
// (sdk+0x1e0f50..0x1e0fbc) would attach handle 0 to the game's event (round41: the game's audio thread spun on
// WaitSynchronization(0)). That path only exists for manual execution, where the client creates its own event.
// For auto execution the host makes the game's event a kernel event and signals it each DSP frame in which the
// renderer is running, as AudioRenderSystem::SendCommandToDsp does with its own event (sdk+0x20c908).
#define MAX_RENDERERS 8
static struct { uint64_t handle; uint32_t writable; int active; } renderers[MAX_RENDERERS];
static pthread_mutex_t renderers_lock = PTHREAD_MUTEX_INITIALIZER;
static void signal_active_renderers(void) {
    uint32_t pending[MAX_RENDERERS]; unsigned n = 0;
    pthread_mutex_lock(&renderers_lock);
    for (unsigned i = 0; i < MAX_RENDERERS; i++) if (renderers[i].writable && renderers[i].active) pending[n++] = renderers[i].writable;
    pthread_mutex_unlock(&renderers_lock);
    for (unsigned i = 0; i < n; i++) wonder_event_signal(pending[i]);
}
HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm",
          "Auto-execution renderer through the in-process server: the game's SystemEvent becomes a kernel event signaled per DSP frame",
          Result, h_open_renderer, uint64_t *handle, void *event, const uint8_t *param, void *buffer, size_t size) {
    uint32_t mode, manual_event; memcpy(&mode, param + 0x2c, 4); memcpy(&manual_event, param + 0x30, 4);
    if (mode == 1 && manual_event == 1) {   // manual execution: the SDK client creates the event itself
        Result (*native)(uint64_t *, void *, const uint8_t *, void *, size_t) =
            required("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm");
        return native(handle, event, param, buffer, size);
    }
    Result (*open4)(uint64_t *, const uint8_t *, void *, size_t) =
        required("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvm");
    Result r = open4(handle, param, buffer, size);
    if (r) { LOG("HOST_AUDIO_RENDERER_OPEN result=%#x", r); return r; }
    Result (*create)(void *, int, unsigned) = required("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    uint32_t (*getw)(const void *) = required("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE");
    r = create(event, 1 /* auto clear, as the SDK client attaches it */, 1 /* inter-process */);
    if (r) die("audio renderer SystemEvent creation failed result=%#x", r);
    uint32_t writable = getw(event);
    pthread_mutex_lock(&renderers_lock);
    unsigned i = 0; while (i < MAX_RENDERERS && renderers[i].writable) i++;
    if (i == MAX_RENDERERS) { pthread_mutex_unlock(&renderers_lock); die("audio: more than %d renderers", MAX_RENDERERS); }
    // handle[0] is the renderer object word of the 16-byte AudioRendererHandle.
    renderers[i].handle = handle[0]; renderers[i].writable = writable; renderers[i].active = 0;
    pthread_mutex_unlock(&renderers_lock);
    LOG("HOST_AUDIO_RENDERER_OPEN handle=%#llx event=%p writable=%#x mode=%u", (unsigned long long)handle[0], event, writable, mode);
    return 0;
}
static void set_renderer_active(uint64_t handle, int active, int remove) {
    pthread_mutex_lock(&renderers_lock);
    for (unsigned i = 0; i < MAX_RENDERERS; i++) if (renderers[i].writable && renderers[i].handle == handle) {
        renderers[i].active = active; if (remove) renderers[i].writable = 0;
    }
    pthread_mutex_unlock(&renderers_lock);
}
HLE_FORCE("_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE", "Track running renderers for per-frame event signals",
          Result, h_start_renderer, uint64_t handle, uint64_t handle_hi) {
    // AudioRendererHandle is 16 bytes passed by value in x0/x1 (sdk+0x1e1090 uses both); x0 is the renderer object.
    Result (*native)(uint64_t, uint64_t) = required("_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE");
    Result r = native(handle, handle_hi); if (!r) set_renderer_active(handle, 1, 0);
    LOG("HOST_AUDIO_RENDERER_START handle=%#llx result=%#x", (unsigned long long)handle, r); return r;
}
HLE_FORCE("_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE", "Track running renderers for per-frame event signals",
          Result, h_stop_renderer, uint64_t handle, uint64_t handle_hi) {
    Result (*native)(uint64_t, uint64_t) = required("_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE");
    set_renderer_active(handle, 0, 0); return native(handle, handle_hi);
}
HLE_FORCE("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE", "Forget the renderer's host event bridge",
          void, h_close_renderer, uint64_t handle, uint64_t handle_hi) {
    void (*native)(uint64_t, uint64_t) = required("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE");
    set_renderer_active(handle, 0, 1); native(handle, handle_hi);
}
