// Host save data for nn::fs (see savedata.h). Layout per user: <root>/save/<uid hi><uid lo>/committed (last
// committed image), working (the mounted copy), committed.new / committed.old (only while a commit is in flight;
// recovered at the next access so a crash leaves either the old or the new image, never a mix).
#include "savedata.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define MAX_MOUNTS 4
#define MAX_FILES 64
#define MAX_DIRS 16
#define MAX_REL 0x300
#define OPEN_READ 1
#define OPEN_WRITE 2
#define OPEN_APPEND 4
#define DIR_DIRECTORIES 1
#define DIR_FILES 2
#define DIR_NO_FILE_SIZE 0x80000000u

static char save_root[PATH_MAX];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct mount { int used; char name[16]; uint64_t uid[2]; char user_dir[PATH_MAX], working[PATH_MAX]; } mounts[MAX_MOUNTS];
static struct file { uint32_t generation; int fd, mode, mount; char rel[MAX_REL + 1]; } files[MAX_FILES];
static struct dir { uint32_t generation; int used, mount; char rel[MAX_REL + 1]; struct wonder_fs_directory_entry *entries; int64_t count, position; } dirs[MAX_DIRS];

int wonder_save_configure(const char *root) {
    if (!root || !*root) return EINVAL;
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/save", root) >= (int)sizeof path) return ENAMETOOLONG;
    if (mkdir(path, 0700) && errno != EEXIST) return errno;
    struct stat st; if (stat(path, &st) || !S_ISDIR(st.st_mode)) return ENOTDIR;
    pthread_mutex_lock(&lock); strcpy(save_root, path); pthread_mutex_unlock(&lock);
    return 0;
}

// ---- host tree helpers (failures are fatal: the save image must never be silently partial) ----
static void join(char *out, const char *a, const char *b) {
    if (snprintf(out, PATH_MAX, "%s%s%s", a, *b ? "/" : "", b) >= PATH_MAX) wonder_save_fatal("save path too long: %s/%s", a, b);
}
static int exists(const char *path, int want_dir) {
    struct stat st; if (lstat(path, &st)) { if (errno == ENOENT || errno == ENOTDIR) return 0; wonder_save_fatal("save stat %s: %s", path, strerror(errno)); }
    return want_dir ? S_ISDIR(st.st_mode) : 1;
}
static void remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st)) { if (errno == ENOENT) return; wonder_save_fatal("save stat %s: %s", path, strerror(errno)); }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path); if (!d) wonder_save_fatal("save opendir %s: %s", path, strerror(errno));
        struct dirent *e; char child[PATH_MAX];
        while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; join(child, path, e->d_name); remove_tree(child); }
        closedir(d);
        if (rmdir(path)) wonder_save_fatal("save rmdir %s: %s", path, strerror(errno));
    } else if (unlink(path)) wonder_save_fatal("save unlink %s: %s", path, strerror(errno));
}
static void sync_path(const char *path) {
    int fd = open(path, O_RDONLY); if (fd < 0) wonder_save_fatal("save open %s: %s", path, strerror(errno));
    if (fsync(fd)) wonder_save_fatal("save fsync %s: %s", path, strerror(errno));
    close(fd);
}
static void copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY); if (in < 0) wonder_save_fatal("save open %s: %s", src, strerror(errno));
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0600); if (out < 0) wonder_save_fatal("save create %s: %s", dst, strerror(errno));
    char buffer[1 << 16];
    for (;;) {
        ssize_t n = read(in, buffer, sizeof buffer);
        if (n < 0) { if (errno == EINTR) continue; wonder_save_fatal("save read %s: %s", src, strerror(errno)); }
        if (!n) break;
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(out, buffer + done, (size_t)(n - done));
            if (w < 0) { if (errno == EINTR) continue; wonder_save_fatal("save write %s: %s", dst, strerror(errno)); }
            done += w;
        }
    }
    if (fsync(out)) wonder_save_fatal("save fsync %s: %s", dst, strerror(errno));
    close(in); close(out);
}
static void copy_tree(const char *src, const char *dst) {
    if (mkdir(dst, 0700)) wonder_save_fatal("save mkdir %s: %s", dst, strerror(errno));
    DIR *d = opendir(src); if (!d) wonder_save_fatal("save opendir %s: %s", src, strerror(errno));
    struct dirent *e; char a[PATH_MAX], b[PATH_MAX];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        join(a, src, e->d_name); join(b, dst, e->d_name);
        struct stat st; if (lstat(a, &st)) wonder_save_fatal("save stat %s: %s", a, strerror(errno));
        if (S_ISDIR(st.st_mode)) copy_tree(a, b);
        else if (S_ISREG(st.st_mode)) copy_file(a, b);
        else wonder_save_fatal("save image contains a non-regular entry %s", a);
    }
    closedir(d); sync_path(dst);
}
static void user_dir(char *out, const uint64_t uid[2]) {
    if (!save_root[0]) wonder_save_fatal("save data used before wonder_save_configure");
    char hex[40]; snprintf(hex, sizeof hex, "%016llx%016llx", (unsigned long long)uid[1], (unsigned long long)uid[0]);
    join(out, save_root, hex);
}
// Finish or roll back an interrupted commit.
static void recover(const char *dir) {
    char committed[PATH_MAX], next[PATH_MAX], old[PATH_MAX];
    join(committed, dir, "committed"); join(next, dir, "committed.new"); join(old, dir, "committed.old");
    if (!exists(committed, 1) && exists(old, 1) && rename(old, committed)) wonder_save_fatal("save recover %s: %s", dir, strerror(errno));
    remove_tree(next); remove_tree(old);
}

uint32_t wonder_save_ensure(const uint64_t uid[2]) {
    if (!uid) return FS_NULLPTR_ARGUMENT;
    pthread_mutex_lock(&lock);
    char dir[PATH_MAX], committed[PATH_MAX]; user_dir(dir, uid); join(committed, dir, "committed");
    if (mkdir(dir, 0700) && errno != EEXIST) wonder_save_fatal("save mkdir %s: %s", dir, strerror(errno));
    recover(dir);
    if (!exists(committed, 1)) { if (mkdir(committed, 0700)) wonder_save_fatal("save mkdir %s: %s", committed, strerror(errno)); sync_path(dir); }
    pthread_mutex_unlock(&lock);
    return 0;
}
uint32_t wonder_save_exists(int *out, const uint64_t uid[2]) {
    if (!out || !uid) return FS_NULLPTR_ARGUMENT;
    pthread_mutex_lock(&lock);
    char dir[PATH_MAX], committed[PATH_MAX]; user_dir(dir, uid); join(committed, dir, "committed");
    if (exists(dir, 1)) recover(dir);
    *out = exists(committed, 1);
    pthread_mutex_unlock(&lock);
    return 0;
}
static int valid_mount_name(const char *name) {
    size_t n = name ? strlen(name) : 0;
    return n && n < sizeof mounts[0].name && !strpbrk(name, ":/\\");
}
static struct mount *find_mount_n(const char *name, size_t n) {
    for (int i = 0; i < MAX_MOUNTS; i++)
        if (mounts[i].used && strlen(mounts[i].name) == n && !strncmp(mounts[i].name, name, n)) return &mounts[i];
    return NULL;
}
static struct mount *find_mount(const char *name) { return name ? find_mount_n(name, strlen(name)) : NULL; }
uint32_t wonder_save_mount(const char *name, const uint64_t uid[2]) {
    if (!name || !uid) return FS_NULLPTR_ARGUMENT;
    if (!valid_mount_name(name)) return FS_INVALID_PATH;
    pthread_mutex_lock(&lock);
    uint32_t result = 0; char dir[PATH_MAX], committed[PATH_MAX];
    user_dir(dir, uid); join(committed, dir, "committed");
    if (find_mount(name)) { result = FS_MOUNT_NAME_ALREADY_EXISTS; goto out; }
    for (int i = 0; i < MAX_MOUNTS; i++)
        if (mounts[i].used && mounts[i].uid[0] == uid[0] && mounts[i].uid[1] == uid[1])
            wonder_save_fatal("save data already mounted as %s; second mount %s would fork the working copy", mounts[i].name, name);
    if (exists(dir, 1)) recover(dir);
    if (!exists(committed, 1)) { result = FS_TARGET_NOT_FOUND; goto out; }
    struct mount *m = NULL;
    for (int i = 0; i < MAX_MOUNTS && !m; i++) if (!mounts[i].used) m = &mounts[i];
    if (!m) wonder_save_fatal("save mount table full (%d mounts)", MAX_MOUNTS);
    memset(m, 0, sizeof *m);
    strcpy(m->name, name); m->uid[0] = uid[0]; m->uid[1] = uid[1]; strcpy(m->user_dir, dir); join(m->working, dir, "working");
    remove_tree(m->working); copy_tree(committed, m->working);
    m->used = 1;
out:
    pthread_mutex_unlock(&lock);
    return result;
}
static int mount_index(const struct mount *m) { return (int)(m - mounts); }
uint32_t wonder_save_commit(const char *name) {
    if (!name) return FS_NULLPTR_ARGUMENT;
    pthread_mutex_lock(&lock);
    uint32_t result = 0; struct mount *m = find_mount(name);
    if (!m) { result = FS_NOT_MOUNTED; goto out; }
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].fd > 0 && files[i].mount == mount_index(m) && (files[i].mode & OPEN_WRITE)) { result = FS_TARGET_LOCKED; goto out; }
    char committed[PATH_MAX], next[PATH_MAX], old[PATH_MAX];
    join(committed, m->user_dir, "committed"); join(next, m->user_dir, "committed.new"); join(old, m->user_dir, "committed.old");
    remove_tree(next); copy_tree(m->working, next);
    if (rename(committed, old)) wonder_save_fatal("save commit %s: %s", committed, strerror(errno));
    if (rename(next, committed)) wonder_save_fatal("save commit %s: %s", next, strerror(errno));
    sync_path(m->user_dir); remove_tree(old);
out:
    pthread_mutex_unlock(&lock);
    return result;
}
uint32_t wonder_save_unmount(const char *name) {
    pthread_mutex_lock(&lock);
    struct mount *m = find_mount(name); uint32_t result = 0;
    if (!m) result = FS_NOT_MOUNTED;
    else {
        for (int i = 0; i < MAX_FILES; i++) if (files[i].fd > 0 && files[i].mount == mount_index(m)) wonder_save_fatal("Unmount %s with file %s open", name, files[i].rel);
        for (int i = 0; i < MAX_DIRS; i++) if (dirs[i].used && dirs[i].mount == mount_index(m)) wonder_save_fatal("Unmount %s with directory /%s open", name, dirs[i].rel);
        remove_tree(m->working); m->used = 0;
    }
    pthread_mutex_unlock(&lock);
    return result;
}
int wonder_save_owns_mount(const char *name) { pthread_mutex_lock(&lock); int r = find_mount(name) != NULL; pthread_mutex_unlock(&lock); return r; }
int wonder_save_owns_path(const char *path) {
    const char *colon = path ? strchr(path, ':') : NULL; if (!colon) return 0;
    pthread_mutex_lock(&lock); int r = find_mount_n(path, (size_t)(colon - path)) != NULL; pthread_mutex_unlock(&lock); return r;
}
int wonder_save_owns_handle(uint64_t handle) { uint32_t slot = (uint32_t)handle; return slot >= WONDER_SAVE_SLOT_BASE; }

// "<mount>:/a/b" -> mount + validated relative path ("" = root). Called with the lock held.
static uint32_t resolve(const char *path, struct mount **m_out, char *rel, char *host) {
    if (!path) return FS_NULLPTR_ARGUMENT;
    const char *colon = strchr(path, ':'); if (!colon) return FS_INVALID_PATH;
    struct mount *m = find_mount_n(path, (size_t)(colon - path)); if (!m) return FS_NOT_MOUNTED;
    const char *p = colon + 1; if (*p != '/') return FS_INVALID_PATH;
    while (*p == '/') p++;
    size_t n = strlen(p); if (n > MAX_REL) return FS_INVALID_PATH;
    memcpy(rel, p, n + 1); while (n && rel[n - 1] == '/') rel[--n] = 0;
    for (char *s = rel; *s;) {
        char *e = strchr(s, '/'); size_t len = e ? (size_t)(e - s) : strlen(s);
        if (!len || (len == 1 && s[0] == '.') || (len == 2 && !memcmp(s, "..", 2))) return FS_INVALID_PATH;
        for (size_t i = 0; i < len; i++) if (s[i] == ':' || s[i] == '\\') return FS_INVALID_PATH;
        if (!e) break;
        s = e + 1;
    }
    *m_out = m; join(host, m->working, rel);
    return 0;
}
static int parent_is_dir(const char *host) {
    char parent[PATH_MAX]; strcpy(parent, host); char *slash = strrchr(parent, '/'); if (!slash) return 0;
    *slash = 0; return exists(parent, 1);
}
static int file_open(int mount, const char *rel, int write_only) {
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].fd > 0 && files[i].mount == mount && !strcmp(files[i].rel, rel) && (!write_only || (files[i].mode & OPEN_WRITE))) return 1;
    return 0;
}
static int open_under(int mount, const char *rel) {
    size_t n = strlen(rel);
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].fd > 0 && files[i].mount == mount && (!n || (!strncmp(files[i].rel, rel, n) && (files[i].rel[n] == '/' || !files[i].rel[n])))) return 1;
    for (int i = 0; i < MAX_DIRS; i++)
        if (dirs[i].used && dirs[i].mount == mount && (!n || (!strncmp(dirs[i].rel, rel, n) && (dirs[i].rel[n] == '/' || !dirs[i].rel[n])))) return 1;
    return 0;
}
uint32_t wonder_save_create_file(const char *path, int64_t size) {
    if (size < 0) return FS_INVALID_SIZE;
    pthread_mutex_lock(&lock);
    struct mount *m; char rel[MAX_REL + 1], host[PATH_MAX]; uint32_t r = resolve(path, &m, rel, host);
    if (!r && !*rel) r = FS_PATH_ALREADY_EXISTS;
    if (!r && exists(host, 0)) r = FS_PATH_ALREADY_EXISTS;
    if (!r && !parent_is_dir(host)) r = FS_PATH_NOT_FOUND;
    if (!r) {
        int fd = open(host, O_WRONLY | O_CREAT | O_EXCL, 0600); if (fd < 0) wonder_save_fatal("save create %s: %s", host, strerror(errno));
        if (ftruncate(fd, (off_t)size) || fsync(fd)) wonder_save_fatal("save size %s: %s", host, strerror(errno));
        close(fd);
    }
    pthread_mutex_unlock(&lock);
    return r;
}
uint32_t wonder_save_delete_file(const char *path) {
    pthread_mutex_lock(&lock);
    struct mount *m; char rel[MAX_REL + 1], host[PATH_MAX]; uint32_t r = resolve(path, &m, rel, host);
    if (!r && (!*rel || !exists(host, 0) || exists(host, 1))) r = FS_PATH_NOT_FOUND;
    if (!r && file_open(mount_index(m), rel, 0)) r = FS_TARGET_LOCKED;
    if (!r && unlink(host)) wonder_save_fatal("save unlink %s: %s", host, strerror(errno));
    pthread_mutex_unlock(&lock);
    return r;
}
uint32_t wonder_save_create_directory(const char *path) {
    pthread_mutex_lock(&lock);
    struct mount *m; char rel[MAX_REL + 1], host[PATH_MAX]; uint32_t r = resolve(path, &m, rel, host);
    if (!r && (!*rel || exists(host, 0))) r = FS_PATH_ALREADY_EXISTS;
    if (!r && !parent_is_dir(host)) r = FS_PATH_NOT_FOUND;
    if (!r && mkdir(host, 0700)) wonder_save_fatal("save mkdir %s: %s", host, strerror(errno));
    pthread_mutex_unlock(&lock);
    return r;
}
uint32_t wonder_save_delete_directory_recursively(const char *path) {
    pthread_mutex_lock(&lock);
    struct mount *m; char rel[MAX_REL + 1], host[PATH_MAX]; uint32_t r = resolve(path, &m, rel, host);
    if (!r && !*rel) r = FS_INVALID_PATH;   // the save root itself cannot be deleted
    if (!r && !exists(host, 1)) r = FS_PATH_NOT_FOUND;
    if (!r && open_under(mount_index(m), rel)) r = FS_TARGET_LOCKED;
    if (!r) remove_tree(host);
    pthread_mutex_unlock(&lock);
    return r;
}
uint32_t wonder_save_entry_type(int32_t *out, const char *path) {
    if (!out) return FS_NULLPTR_ARGUMENT;
    pthread_mutex_lock(&lock);
    struct mount *m; char rel[MAX_REL + 1], host[PATH_MAX]; uint32_t r = resolve(path, &m, rel, host);
    if (!r) { if (!exists(host, 0)) r = FS_PATH_NOT_FOUND; else *out = exists(host, 1) ? 0 : 1; }
    pthread_mutex_unlock(&lock);
    return r;
}
uint32_t wonder_save_open(uint64_t *out, const char *path, int mode) {
    if (!out) return FS_NULLPTR_ARGUMENT;
    if ((mode & ~(OPEN_READ | OPEN_WRITE | OPEN_APPEND)) || !(mode & (OPEN_READ | OPEN_WRITE)) || ((mode & OPEN_APPEND) && !(mode & OPEN_WRITE))) return FS_INVALID_OPEN_MODE;
    pthread_mutex_lock(&lock);
    struct mount *m; char rel[MAX_REL + 1], host[PATH_MAX]; uint32_t r = resolve(path, &m, rel, host);
    if (!r && (!*rel || !exists(host, 0) || exists(host, 1))) r = FS_PATH_NOT_FOUND;
    if (!r && file_open(mount_index(m), rel, !(mode & OPEN_WRITE))) r = FS_TARGET_LOCKED;
    if (!r) {
        int slot = -1; for (int i = 0; i < MAX_FILES && slot < 0; i++) if (files[i].fd <= 0) slot = i;
        if (slot < 0) wonder_save_fatal("save file table full (%d open files)", MAX_FILES);
        int fd = open(host, (mode & OPEN_WRITE) ? O_RDWR : O_RDONLY); if (fd <= 0) wonder_save_fatal("save open %s: %s", host, strerror(errno));
        struct file *f = &files[slot];
        if (++f->generation == 0) ++f->generation;
        f->fd = fd; f->mode = mode; f->mount = mount_index(m); strcpy(f->rel, rel);
        *out = ((uint64_t)f->generation << 32) | (WONDER_SAVE_SLOT_BASE + (uint32_t)slot);
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static struct file *file_of(uint64_t h, const char *who) {
    uint32_t slot = (uint32_t)h - WONDER_SAVE_SLOT_BASE;
    if ((uint32_t)h < WONDER_SAVE_SLOT_BASE || slot >= MAX_FILES || files[slot].fd <= 0 || files[slot].generation != (uint32_t)(h >> 32))
        wonder_save_fatal("save %s on invalid file handle %#llx", who, (unsigned long long)h);
    return &files[slot];
}
static int64_t file_size(int fd, const char *rel) {
    struct stat st; if (fstat(fd, &st)) wonder_save_fatal("save fstat %s: %s", rel, strerror(errno));
    return (int64_t)st.st_size;
}
uint32_t wonder_save_read(size_t *out, uint64_t h, int64_t offset, void *buf, size_t size) {
    if (!out || (!buf && size)) return FS_NULLPTR_ARGUMENT;
    pthread_mutex_lock(&lock);
    struct file *f = file_of(h, "ReadFile"); uint32_t r = 0;
    if (!(f->mode & OPEN_READ)) r = FS_READ_NOT_PERMITTED;
    else if (offset < 0) r = FS_INVALID_OFFSET;
    else {
        int64_t total = file_size(f->fd, f->rel);
        if (offset > total) r = FS_OUT_OF_RANGE;
        else {
            size_t want = (uint64_t)(total - offset) < size ? (size_t)(total - offset) : size, done = 0;
            while (done < want) {
                ssize_t n = pread(f->fd, (char *)buf + done, want - done, (off_t)(offset + (int64_t)done));
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) wonder_save_fatal("save read %s: %s", f->rel, n ? strerror(errno) : "short");
                done += (size_t)n;
            }
            *out = done;
        }
    }
    pthread_mutex_unlock(&lock);
    return r;
}
uint32_t wonder_save_write(uint64_t h, int64_t offset, const void *buf, size_t size, int flush) {
    if (!buf && size) return FS_NULLPTR_ARGUMENT;
    pthread_mutex_lock(&lock);
    struct file *f = file_of(h, "WriteFile"); uint32_t r = 0;
    if (!(f->mode & OPEN_WRITE)) r = FS_WRITE_NOT_PERMITTED;
    else if (offset < 0) r = FS_INVALID_OFFSET;
    else if (!(f->mode & OPEN_APPEND) && (uint64_t)offset + size > (uint64_t)file_size(f->fd, f->rel)) r = FS_FILE_EXTENSION_WITHOUT_APPEND;
    else {
        for (size_t done = 0; done < size;) {
            ssize_t n = pwrite(f->fd, (const char *)buf + done, size - done, (off_t)(offset + (int64_t)done));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) wonder_save_fatal("save write %s: %s", f->rel, n ? strerror(errno) : "short");
            done += (size_t)n;
        }
        if (flush && fsync(f->fd)) wonder_save_fatal("save fsync %s: %s", f->rel, strerror(errno));
    }
    pthread_mutex_unlock(&lock);
    return r;
}
uint32_t wonder_save_size(int64_t *out, uint64_t h) {
    if (!out) return FS_NULLPTR_ARGUMENT;
    pthread_mutex_lock(&lock); struct file *f = file_of(h, "GetFileSize"); *out = file_size(f->fd, f->rel); pthread_mutex_unlock(&lock);
    return 0;
}
uint32_t wonder_save_set_size(uint64_t h, int64_t size) {
    pthread_mutex_lock(&lock);
    struct file *f = file_of(h, "SetFileSize"); uint32_t r = 0;
    if (!(f->mode & OPEN_WRITE)) r = FS_WRITE_NOT_PERMITTED;
    else if (size < 0) r = FS_INVALID_SIZE;
    else if (ftruncate(f->fd, (off_t)size)) wonder_save_fatal("save truncate %s: %s", f->rel, strerror(errno));
    pthread_mutex_unlock(&lock);
    return r;
}
uint32_t wonder_save_flush(uint64_t h) {
    pthread_mutex_lock(&lock);
    struct file *f = file_of(h, "FlushFile");
    if ((f->mode & OPEN_WRITE) && fsync(f->fd)) wonder_save_fatal("save fsync %s: %s", f->rel, strerror(errno));
    pthread_mutex_unlock(&lock);
    return 0;
}
uint32_t wonder_save_close(uint64_t h) {
    pthread_mutex_lock(&lock);
    struct file *f = file_of(h, "CloseFile");
    if ((f->mode & OPEN_WRITE) && fsync(f->fd)) wonder_save_fatal("save fsync %s: %s", f->rel, strerror(errno));
    close(f->fd); f->fd = 0;
    pthread_mutex_unlock(&lock);
    return 0;
}

static int entry_order(const void *a, const void *b) {
    return strcmp(((const struct wonder_fs_directory_entry *)a)->name, ((const struct wonder_fs_directory_entry *)b)->name);
}
uint32_t wonder_save_open_directory(uint64_t *out, const char *path, int mode) {
    if (!out) return FS_NULLPTR_ARGUMENT;
    unsigned kinds = (unsigned)mode & ~DIR_NO_FILE_SIZE;
    if (!kinds || (kinds & ~(unsigned)(DIR_DIRECTORIES | DIR_FILES))) return FS_INVALID_OPEN_MODE;
    pthread_mutex_lock(&lock);
    struct mount *m; char rel[MAX_REL + 1], host[PATH_MAX]; uint32_t r = resolve(path, &m, rel, host);
    if (!r && !exists(host, 1)) r = FS_PATH_NOT_FOUND;
    if (!r) {
        int slot = -1; for (int i = 0; i < MAX_DIRS && slot < 0; i++) if (!dirs[i].used) slot = i;
        if (slot < 0) wonder_save_fatal("save directory table full (%d open)", MAX_DIRS);
        DIR *d = opendir(host); if (!d) wonder_save_fatal("save opendir %s: %s", host, strerror(errno));
        struct wonder_fs_directory_entry *list = NULL; int64_t count = 0, cap = 0; struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char child[PATH_MAX]; join(child, host, e->d_name);
            struct stat st; if (lstat(child, &st)) wonder_save_fatal("save stat %s: %s", child, strerror(errno));
            int is_dir = S_ISDIR(st.st_mode);
            if (!(kinds & (is_dir ? DIR_DIRECTORIES : DIR_FILES))) continue;
            if (strlen(e->d_name) > 0x300) wonder_save_fatal("save entry name too long in %s", host);
            if (count == cap) { cap = cap ? cap * 2 : 16; list = realloc(list, (size_t)cap * sizeof *list); if (!list) wonder_save_fatal("save directory listing out of memory"); }
            memset(&list[count], 0, sizeof *list);
            strcpy(list[count].name, e->d_name); list[count].type = is_dir ? 0 : 1; list[count].size = is_dir ? 0 : (int64_t)st.st_size;
            count++;
        }
        closedir(d);
        if (count) qsort(list, (size_t)count, sizeof *list, entry_order);
        struct dir *dd = &dirs[slot];
        if (++dd->generation == 0) ++dd->generation;
        dd->used = 1; dd->mount = mount_index(m); strcpy(dd->rel, rel); dd->entries = list; dd->count = count; dd->position = 0;
        *out = ((uint64_t)dd->generation << 32) | (WONDER_SAVE_SLOT_BASE + (uint32_t)slot);
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static struct dir *dir_of(uint64_t h, const char *who) {
    uint32_t slot = (uint32_t)h - WONDER_SAVE_SLOT_BASE;
    if ((uint32_t)h < WONDER_SAVE_SLOT_BASE || slot >= MAX_DIRS || !dirs[slot].used || dirs[slot].generation != (uint32_t)(h >> 32))
        wonder_save_fatal("save %s on invalid directory handle %#llx", who, (unsigned long long)h);
    return &dirs[slot];
}
uint32_t wonder_save_read_directory(int64_t *count, struct wonder_fs_directory_entry *entries, uint64_t h, int64_t max) {
    if (!count || (!entries && max > 0)) return FS_NULLPTR_ARGUMENT;
    if (max < 0) return FS_INVALID_SIZE;
    pthread_mutex_lock(&lock);
    struct dir *d = dir_of(h, "ReadDirectory");
    int64_t n = d->count - d->position; if (n > max) n = max;
    if (n) memcpy(entries, d->entries + d->position, (size_t)n * sizeof *entries);
    d->position += n; *count = n;
    pthread_mutex_unlock(&lock);
    return 0;
}
uint32_t wonder_save_directory_entry_count(int64_t *out, uint64_t h) {
    if (!out) return FS_NULLPTR_ARGUMENT;
    pthread_mutex_lock(&lock); *out = dir_of(h, "GetDirectoryEntryCount")->count; pthread_mutex_unlock(&lock);
    return 0;
}
uint32_t wonder_save_close_directory(uint64_t h) {
    pthread_mutex_lock(&lock);
    struct dir *d = dir_of(h, "CloseDirectory"); free(d->entries); d->entries = NULL; d->used = 0;
    pthread_mutex_unlock(&lock);
    return 0;
}
