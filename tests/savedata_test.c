// Host save data: journaled commit/unmount semantics, nn::fs result codes, handles and listings.
#include "savedata.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
_Noreturn void wonder_save_fatal(const char *format, ...) { va_list ap; va_start(ap, format); vfprintf(stderr, format, ap); va_end(ap); fputc('\n', stderr); exit(3); }
#define OK(x) do { uint32_t r_ = (x); if (r_) { fprintf(stderr, "%s:%d %s = %#x\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)
#define EQ(x, v) do { uint32_t r_ = (x); if (r_ != (v)) { fprintf(stderr, "%s:%d %s = %#x want %#x\n", __FILE__, __LINE__, #x, r_, (unsigned)(v)); exit(1); } } while (0)
static const uint64_t uid[2] = {0x1111, 0x2222};
static void write_all(const char *path, const char *text, int mode) {
    int32_t type; if (wonder_save_entry_type(&type, path) == FS_PATH_NOT_FOUND) OK(wonder_save_create_file(path, 0));
    uint64_t h; OK(wonder_save_open(&h, path, mode)); OK(wonder_save_write(h, 0, text, strlen(text), 1)); OK(wonder_save_close(h));
}
static void expect(const char *path, const char *text) {
    uint64_t h; char buf[64] = {0}; size_t got; int64_t size;
    OK(wonder_save_open(&h, path, 1)); OK(wonder_save_size(&size, h)); assert(size == (int64_t)strlen(text));
    OK(wonder_save_read(&got, h, 0, buf, sizeof buf)); assert(got == strlen(text) && !memcmp(buf, text, got));
    EQ(wonder_save_read(&got, h, size + 1, buf, 1), FS_OUT_OF_RANGE);
    OK(wonder_save_read(&got, h, size, buf, 1)); assert(got == 0);
    EQ(wonder_save_write(h, 0, "x", 1, 0), FS_WRITE_NOT_PERMITTED);
    OK(wonder_save_close(h));
}
int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!wonder_save_configure(argv[1]));
    int e = 1; OK(wonder_save_exists(&e, uid)); assert(!e);
    EQ(wonder_save_mount("save", uid), FS_TARGET_NOT_FOUND);
    OK(wonder_save_ensure(uid)); OK(wonder_save_exists(&e, uid)); assert(e);
    EQ(wonder_save_mount("bad:name", uid), FS_INVALID_PATH);
    OK(wonder_save_mount("save", uid)); assert(wonder_save_owns_mount("save") && wonder_save_owns_path("save:/x") && !wonder_save_owns_path("rom:/x"));
    EQ(wonder_save_mount("save", uid), FS_MOUNT_NAME_ALREADY_EXISTS);
    // Files: create, append rules, sizes, invalid paths, open modes, locking.
    OK(wonder_save_create_file("save:/a.bin", 0)); EQ(wonder_save_create_file("save:/a.bin", 0), FS_PATH_ALREADY_EXISTS);
    EQ(wonder_save_create_file("save:/no/a.bin", 0), FS_PATH_NOT_FOUND);
    EQ(wonder_save_create_file("save:/../x", 0), FS_INVALID_PATH); EQ(wonder_save_create_file("save:x", 0), FS_INVALID_PATH);
    EQ(wonder_save_create_file("other:/x", 0), FS_NOT_MOUNTED);
    uint64_t h, h2; EQ(wonder_save_open(&h, "save:/a.bin", 4), FS_INVALID_OPEN_MODE); EQ(wonder_save_open(&h, "save:/a.bin", 8 | 1), FS_INVALID_OPEN_MODE);
    EQ(wonder_save_open(&h, "save:/missing", 1), FS_PATH_NOT_FOUND);
    OK(wonder_save_open(&h, "save:/a.bin", 2)); assert(wonder_save_owns_handle(h));
    EQ(wonder_save_write(h, 0, "abc", 3, 0), FS_FILE_EXTENSION_WITHOUT_APPEND);
    EQ(wonder_save_open(&h2, "save:/a.bin", 1), FS_TARGET_LOCKED);
    EQ(wonder_save_delete_file("save:/a.bin"), FS_TARGET_LOCKED);
    size_t got; char b[4]; EQ(wonder_save_read(&got, h, 0, b, 1), FS_READ_NOT_PERMITTED);
    OK(wonder_save_set_size(h, 3)); OK(wonder_save_write(h, 0, "abc", 3, 0)); OK(wonder_save_flush(h)); OK(wonder_save_close(h));
    EQ(wonder_save_commit("nope"), FS_NOT_MOUNTED);
    OK(wonder_save_open(&h, "save:/a.bin", 6)); EQ(wonder_save_commit("save"), FS_TARGET_LOCKED); OK(wonder_save_write(h, 3, "def", 3, 0)); OK(wonder_save_close(h));
    expect("save:/a.bin", "abcdef");
    OK(wonder_save_open(&h, "save:/a.bin", 1)); OK(wonder_save_open(&h2, "save:/a.bin", 1)); OK(wonder_save_close(h)); OK(wonder_save_close(h2));
    // Directories and listings (sorted, filtered by mode, sizes).
    OK(wonder_save_create_directory("save:/dir")); EQ(wonder_save_create_directory("save:/dir"), FS_PATH_ALREADY_EXISTS);
    write_all("save:/dir/z.txt", "", 2); OK(wonder_save_create_file("save:/dir/y.txt", 5)); OK(wonder_save_create_directory("save:/dir/sub"));
    int32_t type; OK(wonder_save_entry_type(&type, "save:/dir")); assert(type == 0); OK(wonder_save_entry_type(&type, "save:/dir/y.txt")); assert(type == 1);
    OK(wonder_save_entry_type(&type, "save:/")); assert(type == 0); EQ(wonder_save_entry_type(&type, "save:/q"), FS_PATH_NOT_FOUND);
    struct wonder_fs_directory_entry ent[4]; int64_t n;
    OK(wonder_save_open_directory(&h, "save:/dir", 3)); OK(wonder_save_directory_entry_count(&n, h)); assert(n == 3);
    OK(wonder_save_read_directory(&n, ent, h, 2)); assert(n == 2 && !strcmp(ent[0].name, "sub") && ent[0].type == 0 && !strcmp(ent[1].name, "y.txt") && ent[1].type == 1 && ent[1].size == 5);
    OK(wonder_save_read_directory(&n, ent, h, 4)); assert(n == 1 && !strcmp(ent[0].name, "z.txt"));
    OK(wonder_save_read_directory(&n, ent, h, 4)); assert(n == 0);
    EQ(wonder_save_delete_directory_recursively("save:/dir"), FS_TARGET_LOCKED);
    OK(wonder_save_close_directory(h));
    OK(wonder_save_open_directory(&h, "save:/dir", 2)); OK(wonder_save_directory_entry_count(&n, h)); assert(n == 2); OK(wonder_save_close_directory(h));
    EQ(wonder_save_open_directory(&h, "save:/a.bin", 3), FS_PATH_NOT_FOUND); EQ(wonder_save_open_directory(&h, "save:/", 0), FS_INVALID_OPEN_MODE);
    EQ(wonder_save_delete_directory_recursively("save:/"), FS_INVALID_PATH);
    OK(wonder_save_commit("save"));
    // Uncommitted changes vanish at unmount; committed ones survive a remount.
    OK(wonder_save_delete_directory_recursively("save:/dir")); write_all("save:/a.bin", "XYZ", 2); OK(wonder_save_create_file("save:/tmp", 1));
    OK(wonder_save_unmount("save")); EQ(wonder_save_unmount("save"), FS_NOT_MOUNTED);
    OK(wonder_save_mount("save", uid));
    expect("save:/a.bin", "abcdef"); OK(wonder_save_entry_type(&type, "save:/dir/sub")); EQ(wonder_save_entry_type(&type, "save:/tmp"), FS_PATH_NOT_FOUND);
    OK(wonder_save_delete_file("save:/a.bin")); EQ(wonder_save_delete_file("save:/a.bin"), FS_PATH_NOT_FOUND); EQ(wonder_save_delete_file("save:/dir"), FS_PATH_NOT_FOUND);
    OK(wonder_save_commit("save")); OK(wonder_save_unmount("save"));
    // Interrupted commit: committed renamed away but the new image never landed -> the old image is restored.
    char base[1024], a[1100], c[1100];
    snprintf(base, sizeof base, "%s/save/%016llx%016llx", argv[1], (unsigned long long)uid[1], (unsigned long long)uid[0]);
    snprintf(a, sizeof a, "%s/committed", base); snprintf(c, sizeof c, "%s/committed.old", base);
    assert(!rename(a, c)); snprintf(a, sizeof a, "%s/committed.new", base); assert(!mkdir(a, 0700));
    OK(wonder_save_mount("save", uid)); OK(wonder_save_entry_type(&type, "save:/dir/sub")); EQ(wonder_save_entry_type(&type, "save:/a.bin"), FS_PATH_NOT_FOUND);
    struct stat st; assert(stat(a, &st) && stat(c, &st));
    OK(wonder_save_unmount("save"));
    puts("savedata OK");
    return 0;
}
