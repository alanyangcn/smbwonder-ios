// nn::nfp (amiibo) on a host without an NFC reader: the library initializes, the availability event exists
// but never fires, and ListDevices reports no device (nn::nfp::ResultNfcDeviceNotFound, 2115-0064).
// Every per-device API is left to the SDK client: reaching one would mean a device was invented, and it stops.
#include "hle.h"
#define RESULT_NFC_DEVICE_NOT_FOUND 0x8073u   // module 115 (nfp), description 64
#define RESULT_INVALID_STATE 0xfa01u
static int nfp_ready;
static void *required(const char *name) { void *p = guest_sym(name); if (!p) die("nfp missing SDK entry %s", name); return p; }
HLE_FORCE("_ZN2nn3nfp10InitializeEv", "No NFC reader on iPhone host: amiibo library initializes with zero devices", void, h_nfp_init, void) {
    nfp_ready = 1; LOG("HOST_NFP_READY devices=0 (no NFC reader)");
}
HLE_FORCE("_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", "Native SDK SystemEvent that never fires: NFC availability never changes",
          Result, h_nfp_attach_event, void *event) {
    if (!nfp_ready) return RESULT_INVALID_STATE;
    if (!event) die("nfp::AttachAvailabilityChangeEvent event=NULL");
    uint32_t (*create)(void *, int, unsigned) = required("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    Result r = create(event, 0 /* manual clear */, 1 /* inter-process */);
    LOG("HOST_NFP_AVAILABILITY_EVENT event=%p result=%#x (never signaled)", event, r);
    return r;
}
HLE_FORCE("_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", "No NFC reader: zero devices", Result, h_nfp_list, void *handles, int *count, int max) {
    (void)handles; (void)max;
    if (!nfp_ready) return RESULT_INVALID_STATE;
    if (count) *count = 0;
    return RESULT_NFC_DEVICE_NOT_FOUND;
}
