#pragma once
#include <stdint.h>
#include <stddef.h>
// Host save data for nn::fs. One directory per user under <root>/save/<uid hex>/ holding the committed image
// ("committed"). A mount works on a private copy ("working"); Commit atomically replaces the committed image with
// the working copy and Unmount discards uncommitted changes, matching the journaled save data semantics.
// File handles use slots >= WONDER_SAVE_SLOT_BASE so they never collide with RomFS handles (slots 1..255).
#define WONDER_SAVE_SLOT_BASE 0x100u
_Noreturn void wonder_save_fatal(const char *format, ...) __attribute__((format(printf, 1, 2)));  // supplied by the host
int wonder_save_configure(const char *root);                        // errno; root must exist
uint32_t wonder_save_ensure(const uint64_t uid[2]);                  // create empty save data if missing
uint32_t wonder_save_exists(int *out, const uint64_t uid[2]);
uint32_t wonder_save_remove(const uint64_t uid[2]);                 // whole user directory; TARGET_NOT_FOUND if none
uint32_t wonder_save_mount(const char *name, const uint64_t uid[2]);
uint32_t wonder_save_commit(const char *name);
uint32_t wonder_save_unmount(const char *name);                     // NOT_MOUNTED when the name is not a save mount
int wonder_save_owns_path(const char *path);                         // path is "<save mount>:/..."
int wonder_save_owns_mount(const char *name);
int wonder_save_owns_handle(uint64_t handle);
uint32_t wonder_save_create_file(const char *path, int64_t size);
uint32_t wonder_save_delete_file(const char *path);
uint32_t wonder_save_create_directory(const char *path);
uint32_t wonder_save_delete_directory_recursively(const char *path);
uint32_t wonder_save_entry_type(int32_t *out, const char *path);    // 0 directory, 1 file
uint32_t wonder_save_open(uint64_t *out, const char *path, int mode);   // mode: 1 read, 2 write, 4 allow append
// Invalid handles and host I/O failures stop the guest by name (wonder_save_fatal); they are never reported as
// guest-visible results.
uint32_t wonder_save_read(size_t *out, uint64_t h, int64_t offset, void *buf, size_t size);
uint32_t wonder_save_write(uint64_t h, int64_t offset, const void *buf, size_t size, int flush);
uint32_t wonder_save_size(int64_t *out, uint64_t h);
uint32_t wonder_save_set_size(uint64_t h, int64_t size);
uint32_t wonder_save_flush(uint64_t h);
uint32_t wonder_save_close(uint64_t h);
// nn::fs::DirectoryEntry: char name[0x301]; u8 reserved[3]; s8 type; u8 reserved2[3]; s64 size (0x310 bytes).
struct wonder_fs_directory_entry { char name[0x301]; uint8_t reserved[3]; int8_t type; uint8_t reserved2[3]; int64_t size; };
uint32_t wonder_save_open_directory(uint64_t *out, const char *path, int mode);  // mode: 1 directories, 2 files
uint32_t wonder_save_read_directory(int64_t *count, struct wonder_fs_directory_entry *entries, uint64_t h, int64_t max);
uint32_t wonder_save_directory_entry_count(int64_t *out, uint64_t h);
uint32_t wonder_save_close_directory(uint64_t h);
// nn::fs result codes (module 2).
#define FS_PATH_NOT_FOUND 0x202u
#define FS_PATH_ALREADY_EXISTS 0x402u
#define FS_TARGET_LOCKED 0xe02u
#define FS_DIRECTORY_NOT_EMPTY 0x1002u
#define FS_MOUNT_NAME_ALREADY_EXISTS 0x7802u
#define FS_TARGET_NOT_FOUND 0x7d402u
#define FS_OUT_OF_RANGE 0x177a02u
#define FS_INVALID_PATH 0x2eea02u
#define FS_INVALID_OFFSET 0x2f5a02u
#define FS_INVALID_SIZE 0x2f5c02u
#define FS_NULLPTR_ARGUMENT 0x2f5e02u
#define FS_INVALID_OPEN_MODE 0x2f7002u
#define FS_FILE_EXTENSION_WITHOUT_APPEND 0x307202u
#define FS_READ_NOT_PERMITTED 0x307402u
#define FS_WRITE_NOT_PERMITTED 0x307602u
#define FS_NOT_MOUNTED 0x35f202u
