// Offline host for the network-facing SDK services the game touches during boot: friends, nifm, aoc, nsd,
// prepo and err. The host is an offline console with one local user (account_hooks.c): no network connection is
// ever available, the friend list is empty, play history is kept locally, play reports are not transmitted and
// there is no downloadable content. Paths that would need a real network connection stop by name.
#include "hle.h"
#include "account_state.h"
#include "sdk_services.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void require_local_user(const wonder_uid *uid, const char *who) {
    if (!uid || !wonder_account_ready() || !wonder_account_is_user(uid)) die("%s: uid is not the local user", who);
}

// ---------------------------------------------------------------- friends
// AsyncContext is 0x28 bytes (nn::os::MutexType + internal pointer). The host owns its whole lifetime (ctor, dtor,
// HasDone are all host functions), so it keeps its state in the object: magic at +0, completed flag at +8.
#define FRIENDS_ASYNC_MAGIC UINT64_C(0x5746524e44414358)   // "WFRNDACX"
#define FRIENDS_RESULT_NOT_STARTED 0xe79u                  // friends (121) desc 7: the SDK's result for HasDone without a request
struct friends_async { uint64_t magic, done, reserved[3]; };
_Static_assert(sizeof(struct friends_async) == 0x28, "nn::friends::AsyncContext");
static _Atomic int friends_initialized;
static struct friends_async *async_of(void *object, const char *who) {
    struct friends_async *c = object;
    if (!c || c->magic != FRIENDS_ASYNC_MAGIC) die("friends %s on an AsyncContext not constructed by the host (%p)", who, object);
    return c;
}
static void require_friends(const char *who) { if (!atomic_load(&friends_initialized)) die("friends %s before nn::friends::Initialize", who); }
HLE_FORCE("_ZN2nn7friends10InitializeEv", "Offline friends service: empty friend list, local play history", Result, h_friends_init, void) {
    atomic_store(&friends_initialized, 1); LOG("HOST_FRIENDS_READY offline (no friends, play history kept locally)"); return 0;
}
HLE_FORCE("_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE", "Host friends never allocates from the guest", void, h_friends_allocator, void *alloc, void *release) {
    if (!alloc || !release) die("friends SetAllocator with a NULL function");
}
HLE_FORCE("_ZN2nn7friends12AsyncContextC1Ev", "Host-owned async context", void, h_friends_async_ctor, void *object) {
    if (!object) die("friends AsyncContext ctor this=NULL");
    memset(object, 0, sizeof(struct friends_async)); ((struct friends_async *)object)->magic = FRIENDS_ASYNC_MAGIC;
}
HLE_FORCE("_ZN2nn7friends12AsyncContextD1Ev", "Host-owned async context", void, h_friends_async_dtor, void *object) {
    memset(async_of(object, "AsyncContext dtor"), 0, sizeof(struct friends_async));
}
HLE_FORCE("_ZN2nn7friends12AsyncContext7HasDoneEPb", "Requests complete synchronously on the offline host", Result, h_friends_async_done, void *object, uint8_t *done) {
    struct friends_async *c = async_of(object, "HasDone");
    if (!done) die("friends HasDone out=NULL");
    if (!c->done) return FRIENDS_RESULT_NOT_STARTED;
    *done = 1; return 0;
}
HLE_FORCE("_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", "The local (empty) friend list is always available", Result, h_friends_ensure,
          void *object, const wonder_uid *uid) {
    require_friends("EnsureFriendListAvailable"); require_local_user(uid, "friends EnsureFriendListAvailable");
    async_of(object, "EnsureFriendListAvailable")->done = 1; return 0;
}
HLE_FORCE("_ZN2nn7friends27CheckFriendListAvailabilityEPbRKNS_7account3UidE", "The local (empty) friend list is available", Result, h_friends_check,
          uint8_t *available, const wonder_uid *uid) {
    require_friends("CheckFriendListAvailability"); require_local_user(uid, "friends CheckFriendListAvailability");
    if (!available) die("friends CheckFriendListAvailability out=NULL");
    *available = 1; return 0;
}
// nn::friends::Friend is 0x200 bytes; the SDK constructor zero-fills it.
HLE_FORCE("_ZN2nn7friends6FriendC1Ev", "Zero-filled Friend record (SDK layout)", void, h_friend_ctor, void *object) {
    if (!object) die("friends Friend ctor this=NULL");
    memset(object, 0, 0x200);
}
HLE_FORCE("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", "Offline local user has no friends", Result, h_friends_list,
          int32_t *count, void *friends, const wonder_uid *uid, int offset, int max, const void *filter) {
    require_friends("GetFriendList"); require_local_user(uid, "friends GetFriendList");
    if (!count || !filter || offset < 0 || max < 0 || (max && !friends)) die("friends GetFriendList invalid arguments");
    *count = 0; return 0;
}
// PlayHistoryRegistrationKey (0x40 bytes) is opaque to the game. The host key carries a magic, the issuing uid
// and the local-play flag, so AddPlayHistory can verify it was issued here for the local user.
struct play_history_key { char magic[8]; wonder_uid uid; uint8_t local_play, reserved[39]; };
_Static_assert(sizeof(struct play_history_key) == 0x40, "nn::friends::PlayHistoryRegistrationKey");
static const char play_key_magic[8] = {'W', 'P', 'H', 'K', 'E', 'Y', '0', '1'};
HLE_FORCE("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", "Host-issued play history key for the local user",
          Result, h_friends_play_key, struct play_history_key *key, const wonder_uid *uid, unsigned local_play) {
    require_friends("GetPlayHistoryRegistrationKey"); require_local_user(uid, "friends GetPlayHistoryRegistrationKey");
    if (!key) die("friends GetPlayHistoryRegistrationKey out=NULL");
    memset(key, 0, sizeof *key); memcpy(key->magic, play_key_magic, 8); key->uid = *uid; key->local_play = (uint8_t)(local_play & 1);
    return 0;
}
// InAppScreenName = { char name[0x40] (UTF-8); LanguageCode language (8 bytes) } = 0x48 bytes.
struct play_history_record { int64_t time; struct play_history_key key; uint8_t own_name[0x48], other_name[0x48]; };
static pthread_mutex_t play_history_lock = PTHREAD_MUTEX_INITIALIZER;
HLE_FORCE("_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", "Append to the local play history file",
          Result, h_friends_add_history, const wonder_uid *uid, const struct play_history_key *key, const void *own_name, const void *other_name) {
    require_friends("AddPlayHistory"); require_local_user(uid, "friends AddPlayHistory");
    if (!key || !own_name || !other_name) die("friends AddPlayHistory NULL argument");
    if (memcmp(key->magic, play_key_magic, 8)) die("friends AddPlayHistory with a key the host did not issue");
    const char *root = wonder_services_root(); if (!root) die("friends AddPlayHistory before the SDK root is configured");
    struct play_history_record record = {.time = (int64_t)time(NULL), .key = *key};
    memcpy(record.own_name, own_name, sizeof record.own_name); memcpy(record.other_name, other_name, sizeof record.other_name);
    char path[1024]; snprintf(path, sizeof path, "%s/friends", root);
    pthread_mutex_lock(&play_history_lock);
    if (mkdir(path, 0700) && errno != EEXIST) die("friends play history mkdir %s failed", path);
    strncat(path, "/play_history.bin", sizeof path - strlen(path) - 1);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0 || write(fd, &record, sizeof record) != (ssize_t)sizeof record || fsync(fd)) die("friends play history write %s failed", path);
    close(fd);
    pthread_mutex_unlock(&play_history_lock);
    return 0;
}

// ---------------------------------------------------------------- nifm
static _Atomic int nifm_initialized, nifm_local_mode;
static void require_nifm(const char *who) { if (!atomic_load(&nifm_initialized)) die("nifm %s before nn::nifm::Initialize", who); }
HLE_FORCE("_ZN2nn4nifm10InitializeEv", "Offline host: the network interface manager never connects", Result, h_nifm_init, void) {
    atomic_store(&nifm_initialized, 1); LOG("HOST_NIFM_READY offline"); return 0;
}
HLE_FORCE("_ZN2nn4nifm19SetLocalNetworkModeEb", "Remember the requested mode; neither mode connects on this host", void, h_nifm_local_mode, unsigned local) {
    require_nifm("SetLocalNetworkMode"); atomic_store(&nifm_local_mode, (int)(local & 1));
}
HLE_FORCE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", "The request is evaluated at once: no network is available", void, h_nifm_submit, void) {
    require_nifm("SubmitNetworkRequestAndWait"); LOG("HOST_NIFM_REQUEST local=%d rejected (offline)", atomic_load(&nifm_local_mode));
}
HLE_FORCE("_ZN2nn4nifm18IsNetworkAvailableEv", "Offline host: never available (returns bool)", unsigned, h_nifm_available, void) {
    require_nifm("IsNetworkAvailable"); return 0;
}
HLE_FORCE("_ZN2nn4nifm20CancelNetworkRequestEv", "No request is ever pending", void, h_nifm_cancel, void) { require_nifm("CancelNetworkRequest"); }
HLE_FORCE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", "No error applet can make the network available: unresolved (returns bool)", unsigned, h_nifm_handle_error, void) {
    require_nifm("HandleNetworkRequestErrorResult"); LOG("HOST_NIFM_ERROR_HANDLING unresolved (offline)"); return 0;
}
HLE_FORCE("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", "Requires a network connection", Result, h_nifm_ip, void *a, void *b, void *c, void *d, void *e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    die("nifm::GetCurrentIpConfigInfo needs a network connection; the host is offline");
}
HLE_FORCE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", "Requires a network connection", Result, h_nifm_proxy, void *out) {
    (void)out;
    die("nifm::GetCurrentProxySetting needs a network connection; the host is offline");
}

// ---------------------------------------------------------------- aoc / nsd / prepo / err
HLE_FORCE("_ZN2nn3aoc17CountAddOnContentEv", "No add-on content is installed on the host", int, h_aoc_count, void) { return 0; }
// EnvironmentIdentifier = char[8]; retail consoles report "lp1".
HLE_FORCE("_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", "Retail environment identifier", void, h_nsd_env, char *out) {
    if (!out) die("nsd GetEnvironmentIdentifier out=NULL");
    memset(out, 0, 8); memcpy(out, "lp1", 3);
}
static _Atomic unsigned long play_reports;
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveEv", "Play reports are not transmitted from the offline host", Result, h_prepo_save, void *report) {
    if (!report) die("prepo Save this=NULL");
    unsigned long n = atomic_fetch_add(&play_reports, 1) + 1; if (n == 1 || !(n % 100)) LOG("HOST_PREPO_DISCARDED count=%lu", n);
    return 0;
}
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", "Play reports are not transmitted from the offline host", Result, h_prepo_save_user, void *report, const wonder_uid *uid) {
    require_local_user(uid, "prepo PlayReport::Save"); return h_prepo_save(report);
}
HLE_FORCE("_ZN2nn5prepo28RequestImmediateTransmissionEv", "Nothing is queued for transmission", Result, h_prepo_transmit, void) { return 0; }
HLE_FORCE("_ZN2nn3err9ShowErrorENS_6ResultE", "Error applet: stop with the result", void, h_err_result, Result result) {
    die("GAME_ERROR nn::err::ShowError result=%#x (module %u description %u)", result, result & 0x1ff, (result >> 9) & 0x1fff);
}
// ErrorCode = { u32 category; u32 number } passed in x0.
HLE_FORCE("_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", "Error applet: stop with the error code", void, h_err_code, uint64_t code) {
    die("GAME_ERROR nn::err::ShowError code=%04u-%04u", (unsigned)(code & 0xffffffff), (unsigned)(code >> 32));
}
HLE_FORCE("_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE", "Error applet: stop with the variant", void, h_err_variant, const uint32_t *variant) {
    if (!variant) die("GAME_ERROR nn::err::ShowError(ErrorResultVariant NULL)");
    die("GAME_ERROR nn::err::ShowError variant=%08x %08x %08x", variant[0], variant[1], variant[2]);
}
