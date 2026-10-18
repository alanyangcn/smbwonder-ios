// nn::account for up to 8 offline local users (account_state.h). Network-service-account and Nintendo Account APIs are not
// implemented here: if the game calls them, the SDK's own client reaches sm: and the guest stops there.
#include "hle.h"
#include "account_state.h"
#include "sdk_services.h"
#include "profile_image.h"
#define RESULT_INVALID_ARGUMENT 0xd401u   // kernel-style invalid-argument code for host-detected misuse
static void require_ready(const char *who) { if (!wonder_account_ready()) die("account: %s before nn::account::Initialize", who); }
// The host normally loads the users before the game starts (user_admin.c, applying queued deletions first).
HLE_FORCE("_ZN2nn7account10InitializeEv", "Host account store: up to 8 offline local users persisted under WonderSDK/account", void, h_account_init, void) {
    int error = wonder_account_initialize(wonder_services_root());
    if (error) die("HOST_ACCOUNT_FAILED error=%d", error);
    wonder_uid list[WONDER_ACCOUNT_MAX_USERS]; int n = wonder_account_list(list, WONDER_ACCOUNT_MAX_USERS);
    LOG("HOST_ACCOUNT_READY users=%d (offline; network accounts unavailable)", n);
    for (int i = 0; i < n; i++) {
        char hex[33], name[WONDER_NICKNAME_MAX + 1]; wonder_uid_hex(&list[i], hex); wonder_account_nickname(&list[i], name);
        LOG("HOST_ACCOUNT_USER %d uid=%s nickname=%s", i, hex, name);
    }
}
HLE_FORCE("_ZN2nn7account12GetUserCountEPi", "Number of local users", Result, h_account_count, int *count) {
    require_ready("GetUserCount"); if (!count) return RESULT_INVALID_ARGUMENT; *count = wonder_account_list(NULL, 0); return 0;
}
// The game lists users for its picker with max 8 (main+0x265654).
HLE_FORCE("_ZN2nn7account18ListQualifiedUsersEPiPNS0_3UidEi", "Every local user qualifies (offline, no restrictions)", Result, h_account_list_qualified, int *count, wonder_uid *out, int max) {
    require_ready("ListQualifiedUsers"); if (!count || (max > 0 && !out)) return RESULT_INVALID_ARGUMENT;
    int n = wonder_account_list(out, max > 0 ? max : 0); *count = n < max ? n : (max > 0 ? max : 0);
    return 0;
}
HLE_FORCE("_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE", "Existence of a local user", Result, h_account_exists, uint8_t *exists, const wonder_uid *uid) {
    require_ready("GetUserExistence"); if (!exists || !uid) return RESULT_INVALID_ARGUMENT;
    *exists = (uint8_t)wonder_account_is_user(uid); return 0;
}
// The game then shows its own picker over all users (main+0x59df24 -> main+0x26562c).
HLE_FORCE("_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE", "Launcher preselects the first local user (returns bool)", unsigned, h_account_try_preselected, wonder_user_handle *handle) {
    require_ready("TryOpenPreselectedUser"); wonder_uid u;
    int ok = handle && wonder_account_list(&u, 1) > 0 && !wonder_account_open(handle, &u);
    LOG("HOST_ACCOUNT_PRESELECTED opened=%d", ok);
    return (unsigned)ok;
}
HLE_FORCE("_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE", "Open a local user", Result, h_account_open, wonder_user_handle *handle, const wonder_uid *uid) {
    require_ready("OpenUser"); return wonder_account_open(handle, uid) ? RESULT_INVALID_ARGUMENT : 0;
}
HLE_FORCE("_ZN2nn7account9CloseUserERKNS0_10UserHandleE", "Close a handle issued by this host", void, h_account_close, const wonder_user_handle *handle) {
    if (wonder_account_close(handle)) die("account::CloseUser on a handle not opened by the host");
}
HLE_FORCE("_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE", "Uid of an open handle", Result, h_account_get_uid, wonder_uid *out, const wonder_user_handle *handle) {
    if (wonder_account_handle_uid(handle, out)) die("account::GetUserId on a handle not opened by the host");
    return 0;
}
// nn::account::Nickname is char[33], UTF-8, NUL-terminated.
HLE_FORCE("_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE", "Nickname of a local user (set in the host settings panel)", Result, h_account_nickname, char *nickname, const wonder_uid *uid) {
    require_ready("GetNickname"); char name[WONDER_NICKNAME_MAX + 1];
    if (!nickname || wonder_account_nickname(uid, name)) return RESULT_INVALID_ARGUMENT;
    memset(nickname, 0, 33); memcpy(nickname, name, strlen(name)); return 0;
}
HLE_FORCE("_ZN2nn7account12PopOpenUsersEPiPNS0_10UserHandleEi", "No launcher-opened users to hand over", Result, h_account_pop_open, int *count, wonder_user_handle *out, int max) {
    require_ready("PopOpenUsers"); if (!count) return RESULT_INVALID_ARGUMENT; *count = wonder_account_pop_open(out, max); return 0;
}
HLE_FORCE("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", "Offline host: the local user has no network service account", Result, h_account_nsa_available, uint8_t *available, const wonder_user_handle *handle) {
    wonder_uid u; if (!available || wonder_account_handle_uid(handle, &u)) return RESULT_INVALID_ARGUMENT;
    *available = 0; return 0;
}
// nn::account::LoadProfileImage(size_t *outSize, void *buffer, size_t bufferSize, const Uid &): the user icon JPEG.
// Offline users have no Nintendo icon; the host supplies one generated placeholder for all of them (profile_image.h). Round88: the
// sdk's own client dereferenced a null service proxy (sdk+0x199e6c), called from main+0xa80e0.
HLE_FORCE("_ZN2nn7account16LoadProfileImageEPmPvmRKNS0_3UidE", "Offline user icon: generated 256x256 JPEG placeholder",
          Result, h_account_profile_image, size_t *out_size, void *buffer, size_t buffer_size, const wonder_uid *uid) {
    require_ready("LoadProfileImage");
    if (!out_size || !uid || !wonder_account_is_user(uid)) return RESULT_INVALID_ARGUMENT;
    size_t n; const unsigned char *jpeg = wonder_profile_image(&n);
    if (!jpeg) die("account::LoadProfileImage: host could not generate the profile image");
    *out_size = n;
    if (buffer) {
        if (buffer_size < n) die("account::LoadProfileImage buffer %#zx smaller than the image %#zx", buffer_size, n);
        memcpy(buffer, jpeg, n);
    }
    static int logged; if (!logged++) LOG("HOST_ACCOUNT_PROFILE_IMAGE %zu bytes (generated placeholder; no Nintendo user icon offline)", n);
    return 0;
}
// nn::account::ShowUserCreator(): the player-select applet in user-creator mode (sdk+0x198390 -> StartPselApplet,
// mode 1). The offline host has no user-creation applet (users are added in the host settings panel), so it never runs; the result is the
// sdk's own when the applet does not complete, ResultCancelledByUser 0x27c (sdk+0x199594), as if the player
// backed out of the creator. Round122: the sdk client dereferenced a null service object in
// IsUserRegistrationRequestPermitted (sdk+0x199fe4), called from main+0x260230.
HLE_FORCE("_ZN2nn7account15ShowUserCreatorEv", "Offline host has no user-creation applet: cancelled by user (0x27c)", Result, h_account_show_creator, void) {
    require_ready("ShowUserCreator");
    LOG("HOST_ACCOUNT_USER_CREATOR not available on the offline host -> ResultCancelledByUser 0x27c");
    return 0x27c;
}
