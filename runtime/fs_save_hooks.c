// nn::fs save data on the host (savedata.c): one journaled save image per local user under WonderSDK/save.
#include "hle.h"
#include "savedata.h"
#include "account_state.h"
#include "sdk_services.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
_Noreturn void wonder_save_fatal(const char *format, ...) {
    char message[512]; va_list ap; va_start(ap, format); vsnprintf(message, sizeof message, format, ap); va_end(ap);
    die("HOST_SAVE_FATAL %s", message);
}
static pthread_once_t configured = PTHREAD_ONCE_INIT;
static void configure(void) {
    const char *root = wonder_services_root(); if (!root) die("save data used before the SDK root is configured");
    int error = wonder_save_configure(root); if (error) die("HOST_SAVE_CONFIGURE_FAILED error=%d", error);
}
static int local_user(const wonder_uid *uid) { pthread_once(&configured, configure); return uid && wonder_account_ready() && wonder_account_is_user(uid); }

HLE_FORCE("_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE", "Create the user's host save image if missing", Result, h_ensure_save, const wonder_uid *uid) {
    if (!local_user(uid)) return FS_TARGET_NOT_FOUND;
    Result r = wonder_save_ensure(uid->v); LOG("SAVE_ENSURE result=%#x", r); return r;
}
HLE_FORCE("_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE", "Host save image presence (returns bool)", unsigned, h_save_exists, const wonder_uid *uid) {
    if (!local_user(uid)) return 0;
    int exists = 0; Result r = wonder_save_exists(&exists, uid->v); if (r) die("IsSaveDataExisting result=%#x", r);
    return (unsigned)exists;
}
HLE_FORCE("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", "Mount a working copy of the user's host save image", Result, h_mount_save, const char *name, const wonder_uid *uid) {
    if (!local_user(uid)) return FS_TARGET_NOT_FOUND;
    Result r = wonder_save_mount(name, uid->v); LOG("SAVE_MOUNT name=%s result=%#x", name ? name : "(null)", r); return r;
}
HLE_FORCE("_ZN2nn2fs6CommitEPKc", "Atomically replace the committed save image with the working copy", Result, h_commit, const char *name) {
    Result r = wonder_save_commit(name); LOG("SAVE_COMMIT name=%s result=%#x", name ? name : "(null)", r); return r;
}
HLE_FORCE("_ZN2nn2fs7UnmountEPKc", "Unmount host save data, discarding uncommitted changes", void, h_unmount, const char *name) {
    Result r = wonder_save_unmount(name);
    if (r) die("fs::Unmount(%s) is not a mounted save data (result %#x); RomFS unmount is not supported", name ? name : "(null)", r);
    LOG("SAVE_UNMOUNT name=%s", name);
}
HLE_FORCE("_ZN2nn2fs10CreateFileEPKcl", "Create a zero-filled file in save data", Result, h_create_file, const char *path, int64_t size) {
    Result r = wonder_save_create_file(path, size); LOG("SAVE_CREATE_FILE path=%s size=%lld result=%#x", path ? path : "(null)", (long long)size, r); return r;
}
HLE_FORCE("_ZN2nn2fs10DeleteFileEPKc", "Delete a save data file", Result, h_delete_file, const char *path) {
    Result r = wonder_save_delete_file(path); LOG("SAVE_DELETE_FILE path=%s result=%#x", path ? path : "(null)", r); return r;
}
HLE_FORCE("_ZN2nn2fs15CreateDirectoryEPKc", "Create a save data directory", Result, h_create_dir, const char *path) {
    Result r = wonder_save_create_directory(path); LOG("SAVE_CREATE_DIR path=%s result=%#x", path ? path : "(null)", r); return r;
}
HLE_FORCE("_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", "Delete a save data directory tree", Result, h_delete_dir, const char *path) {
    Result r = wonder_save_delete_directory_recursively(path); LOG("SAVE_DELETE_DIR path=%s result=%#x", path ? path : "(null)", r); return r;
}
// nn::fs::WriteOption = { int flags }, bit 0 = flush.
HLE_FORCE("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", "Write save data file bytes", Result, h_write_file,
          uint64_t h, int64_t offset, const void *buf, size_t size, const int32_t *option) {
    if (!option) return FS_NULLPTR_ARGUMENT;
    if (!wonder_save_owns_handle(h)) die("fs::WriteFile on read-only RomFS handle %#llx", (unsigned long long)h);
    return wonder_save_write(h, offset, buf, size, *option & 1);
}
HLE_FORCE("_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", "Resize a save data file", Result, h_set_size, uint64_t h, int64_t size) {
    if (!wonder_save_owns_handle(h)) die("fs::SetFileSize on read-only RomFS handle %#llx", (unsigned long long)h);
    return wonder_save_set_size(h, size);
}
HLE_FORCE("_ZN2nn2fs9FlushFileENS0_10FileHandleE", "fsync a save data file (RomFS: nothing buffered)", Result, h_flush, uint64_t h) {
    return wonder_save_owns_handle(h) ? wonder_save_flush(h) : 0;
}
// nn::fs::DirectoryHandle = { void *handle } passed by value.
HLE_FORCE("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", "Snapshot a save data directory listing", Result, h_open_dir, uint64_t *out, const char *path, int mode) {
    Result r = wonder_save_open_directory(out, path, mode); LOG("SAVE_OPEN_DIR path=%s mode=%#x result=%#x", path ? path : "(null)", mode, r); return r;
}
HLE_FORCE("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", "Next entries of the listing", Result, h_read_dir,
          int64_t *count, struct wonder_fs_directory_entry *entries, uint64_t h, int64_t max) { return wonder_save_read_directory(count, entries, h, max); }
HLE_FORCE("_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", "Entries in the listing", Result, h_dir_count, int64_t *out, uint64_t h) {
    return wonder_save_directory_entry_count(out, h);
}
HLE_FORCE("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", "Release the listing", void, h_close_dir, uint64_t h) { wonder_save_close_directory(h); }
_Static_assert(sizeof(struct wonder_fs_directory_entry) == 0x310, "nn::fs::DirectoryEntry");
