// nn::hid service boundary for Wonder. The iPhone is the handheld controller; mouse and keyboard are
// absent (attributes 0). Vibration: IsVibrationPermitted reports false (as with vibration disabled in
// system settings); values sent anyway are discarded, which is what the console does in that case.
// Six-axis: handles exist, sensor states report not-connected until CoreMotion is wired.
// The SDK's user-space vibration classes (VibrationPlayer/Mixer/Node) run unchanged.
#include "hle.h"
#include "hid_state.h"
#define RESULT_INVALID_ARGUMENT 0xd401u
static int npad_ready, mouse_ready, keyboard_ready;
static uint32_t joy_assignment[0x21];
static void need_npad(const char *who) { if (!npad_ready) die("hid: %s before InitializeNpad", who); }
static uint32_t npad_id(const uint32_t *id, const char *who) {
    if (!id || !wonder_hid_valid_npad_id(*id)) die("hid: %s invalid NpadId %#x", who, id ? *id : 0xffffffffu);
    return *id;
}
HLE_FORCE("_ZN2nn3hid14InitializeNpadEv", "Host npad: iPhone handheld controller fed by the host front end", void, h_hid_init_npad, void) {
    if (!npad_ready) wonder_hid_reset();
    npad_ready = 1; LOG("HOST_HID_NPAD_READY handheld=0x20 (inputs neutral until front end publishes)");
}
HLE_FORCE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", "Record supported NpadIds", void, h_hid_ids, const uint32_t *ids, uint64_t count) {
    need_npad("SetSupportedNpadIdType");
    if (wonder_hid_set_supported_ids(ids, count)) die("hid: SetSupportedNpadIdType invalid list (count %llu)", (unsigned long long)count);
    LOG("HOST_HID_SUPPORTED_IDS count=%llu", (unsigned long long)count);
}
HLE_FORCE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "Record supported styles", void, h_hid_styles, uint32_t styles) {
    need_npad("SetSupportedNpadStyleSet"); wonder_hid_set_supported_styles(styles);
    LOG("HOST_HID_SUPPORTED_STYLES %#x handheld_connected=%d", styles, wonder_hid_style_set(0x20) != 0);
}
HLE_FORCE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", "Joy-Con hold type (no Joy-Con on host)", void, h_hid_hold, int type) {
    need_npad("SetNpadJoyHoldType"); if (type != 0 && type != 1) die("hid: SetNpadJoyHoldType %d", type);
}
HLE_FORCE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj", "Joy-Con assignment bookkeeping", void, h_hid_single, const uint32_t *id) {
    need_npad("SetNpadJoyAssignmentModeSingle"); joy_assignment[npad_id(id, "SetNpadJoyAssignmentModeSingle")] = 1;
}
// L/R assignment mode lets console players re-pair Joy-Con by pressing L+R. The host has one built-in
// handheld controller and nothing to re-pair, so the mode only changes this flag (functional difference).
static int lr_assignment_mode;
HLE_FORCE("_ZN2nn3hid21StartLrAssignmentModeEv", "L/R assignment mode: nothing to re-pair on the host handheld", void, h_hid_lr_start, void) {
    lr_assignment_mode = 1; LOG("HOST_HID_LR_ASSIGNMENT start (no effect: built-in handheld)");
}
HLE_FORCE("_ZN2nn3hid20StopLrAssignmentModeEv", "L/R assignment mode: nothing to re-pair on the host handheld", void, h_hid_lr_stop, void) {
    lr_assignment_mode = 0; LOG("HOST_HID_LR_ASSIGNMENT stop");
}
HLE_FORCE("_ZN2nn3hid20GetNpadJoyAssignmentERKj", "Joy-Con assignment bookkeeping", uint32_t, h_hid_get_assignment, const uint32_t *id) {
    need_npad("GetNpadJoyAssignment"); return joy_assignment[npad_id(id, "GetNpadJoyAssignment")];
}
HLE_FORCE("_ZN2nn3hid15GetNpadStyleSetERKj", "Connected style of an npad", uint32_t, h_hid_style, const uint32_t *id) {
    need_npad("GetNpadStyleSet"); return wonder_hid_style_set(npad_id(id, "GetNpadStyleSet"));
}
#define STATES(mangled, style, cname) \
    HLE_FORCE(mangled, "Npad state history from the host input model", int, cname, wonder_npad_state *out, int count, const uint32_t *id) { \
        need_npad(#cname); return wonder_hid_get_states(out, count, npad_id(id, #cname), style); }
STATES("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", WONDER_STYLE_FULLKEY, h_hid_states_fullkey)
STATES("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", WONDER_STYLE_HANDHELD, h_hid_states_handheld)
STATES("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", WONDER_STYLE_JOYDUAL, h_hid_states_joydual)
STATES("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", WONDER_STYLE_JOYLEFT, h_hid_states_joyleft)
STATES("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", WONDER_STYLE_JOYRIGHT, h_hid_states_joyright)
HLE_FORCE("_ZN2nn3hid14DisconnectNpadERKj", "Disconnect request: the iPhone handheld cannot be detached", void, h_hid_disconnect, const uint32_t *id) {
    need_npad("DisconnectNpad"); LOG("HOST_HID_DISCONNECT_REQUEST id=%#x (ignored for built-in handheld)", npad_id(id, "DisconnectNpad"));
}
// nn::hid::MouseState (0x28) / KeyboardState (0x30): no mouse or keyboard attached.
HLE_FORCE("_ZN2nn3hid15InitializeMouseEv", "No mouse on host", void, h_hid_init_mouse, void) { mouse_ready = 1; }
HLE_FORCE("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", "No mouse: one not-connected state", int, h_hid_mouse, uint8_t *out, int count) {
    if (!mouse_ready) die("hid: GetMouseStates before InitializeMouse");
    if (!out || count <= 0) return 0;
    memset(out, 0, 0x28); return 1;
}
HLE_FORCE("_ZN2nn3hid18InitializeKeyboardEv", "No keyboard on host", void, h_hid_init_keyboard, void) { keyboard_ready = 1; }
HLE_FORCE("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", "No keyboard: not-connected state", void, h_hid_keyboard, uint8_t *out) {
    if (!keyboard_ready) die("hid: GetKeyboardState before InitializeKeyboard");
    memset(out, 0, 0x30);
}
// Vibration / six-axis handles: 4-byte values the game only passes back to the functions below.
#define HANDLE_TAG 0x5a000000u
static int handle_count(uint32_t id, uint32_t style) { return id == WONDER_NPAD_HANDHELD && (style & WONDER_STYLE_HANDHELD) && wonder_hid_style_set(id) ? 2 : 0; }
static int valid_handle(const uint32_t *h) { return h && (*h & 0xff000000u) == HANDLE_TAG; }
HLE_FORCE("_ZN2nn3hid20IsVibrationPermittedEv", "Host reports vibration disabled (returns bool)", unsigned, h_hid_vib_permitted, void) { return 0; }
HLE_FORCE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
          "Two vibration devices on the handheld (left/right)", int, h_hid_vib_handles, uint32_t *out, int count, const uint32_t *id, uint32_t style) {
    need_npad("GetVibrationDeviceHandles");
    int n = handle_count(npad_id(id, "GetVibrationDeviceHandles"), style); if (n > count) n = count;
    for (int i = 0; i < n; i++) out[i] = HANDLE_TAG | 0x10000u | (*id << 8) | (uint32_t)i;
    return n;
}
HLE_FORCE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", "Accept handles issued above", void, h_hid_vib_init, const uint32_t *h) {
    if (!valid_handle(h)) die("hid: InitializeVibrationDevice foreign handle");
}
HLE_FORCE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", "Vibration not permitted: value discarded", void, h_hid_vib_send, const uint32_t *h, const void *value) {
    (void)value; if (!valid_handle(h)) die("hid: SendVibrationValue foreign handle");
}
HLE_FORCE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
          "Six-axis handles for the handheld", int, h_hid_sixaxis_handles, uint32_t *out, int count, const uint32_t *id, uint32_t style) {
    need_npad("GetSixAxisSensorHandles");
    int n = handle_count(npad_id(id, "GetSixAxisSensorHandles"), style) ? 1 : 0; if (n > count) n = count;
    if (n) out[0] = HANDLE_TAG | 0x20000u | (*id << 8);
    return n;
}
HLE_FORCE("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", "Accept handle; motion not wired yet", void, h_hid_sixaxis_start, const uint32_t *h) {
    if (!valid_handle(h)) die("hid: StartSixAxisSensor foreign handle");
}
// nn::hid::SixAxisSensorState is 0x60 bytes; attributes 0 = not connected (no CoreMotion feed yet).
HLE_FORCE("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE",
          "Six-axis not connected until CoreMotion is wired", int, h_hid_sixaxis_states, uint8_t *out, int count, const uint32_t *h) {
    if (!valid_handle(h)) die("hid: GetSixAxisSensorStates foreign handle");
    if (!out || count <= 0) return 0;
    memset(out, 0, 0x60); return 1;
}
