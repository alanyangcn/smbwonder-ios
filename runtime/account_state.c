// nn::account host state: single offline local user, persisted in <root>/account.
// Nickname: <root>/account/nickname.txt if present (first line, <= 32 UTF-8 bytes), else "Player".
#include "account_state.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define HANDLE_MAGIC 0x4c444e48524553ull   /* "USERHNDL" tag in v[2] low bytes */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int ready, open_count;
static wonder_uid uid;
static char nickname[33];
static int random_bytes(void *p, size_t n) {
#if defined(__APPLE__)
    arc4random_buf(p, n); return 0;
#else
    int fd = open("/dev/urandom", O_RDONLY); if (fd < 0) return errno;
    ssize_t r = read(fd, p, n); close(fd); return r == (ssize_t)n ? 0 : EIO;
#endif
}
int wonder_account_initialize(const char *root) {
    if (!root) return ENODEV;
    pthread_mutex_lock(&lock);
    int error = 0;
    if (ready) goto done;
    char dir[PATH_MAX], path[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s/account", root) >= (int)sizeof dir) { error = ENAMETOOLONG; goto done; }
    if (mkdir(dir, 0700) && errno != EEXIST) { error = errno; goto done; }
    snprintf(path, sizeof path, "%s/uid.bin", dir);
    FILE *f = fopen(path, "rb");
    if (f) { size_t n = fread(&uid, 1, sizeof uid, f); fclose(f); if (n != sizeof uid || (!uid.v[0] && !uid.v[1])) { error = EINVAL; goto done; } }
    else {
        do { if ((error = random_bytes(&uid, sizeof uid))) goto done; } while (!uid.v[0] && !uid.v[1]);
        char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
        f = fopen(tmp, "wb");
        if (!f || fwrite(&uid, 1, sizeof uid, f) != sizeof uid || fclose(f) || rename(tmp, path)) { error = errno ? errno : EIO; goto done; }
    }
    strcpy(nickname, "Player");
    snprintf(path, sizeof path, "%s/nickname.txt", dir);
    f = fopen(path, "r");
    if (f) {
        char line[64] = {0};
        if (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; if (line[0] && strlen(line) <= 32) strcpy(nickname, line); }
        fclose(f);
    }
    open_count = 0; ready = 1;
done:
    pthread_mutex_unlock(&lock);
    return error;
}
int wonder_account_ready(void) { return ready; }
wonder_uid wonder_account_uid(void) { return uid; }
int wonder_account_is_user(const wonder_uid *u) { return ready && u && u->v[0] == uid.v[0] && u->v[1] == uid.v[1]; }
const char *wonder_account_nickname(void) { return nickname; }
int wonder_account_open(wonder_user_handle *out, const wonder_uid *u) {
    if (!out || !wonder_account_is_user(u)) return -1;
    pthread_mutex_lock(&lock); open_count++; pthread_mutex_unlock(&lock);
    *out = (wonder_user_handle){{u->v[0], u->v[1], HANDLE_MAGIC}};
    return 0;
}
static int valid_handle(const wonder_user_handle *h) { return h && h->v[2] == HANDLE_MAGIC && h->v[0] == uid.v[0] && h->v[1] == uid.v[1]; }
int wonder_account_close(const wonder_user_handle *h) {
    pthread_mutex_lock(&lock);
    int ok = valid_handle(h) && open_count > 0;
    if (ok) open_count--;
    pthread_mutex_unlock(&lock);
    return ok ? 0 : -1;
}
int wonder_account_handle_uid(const wonder_user_handle *h, wonder_uid *out) {
    if (!valid_handle(h) || !out) return -1;
    *out = uid; return 0;
}
int wonder_account_pop_open(wonder_user_handle *out, int max) {
    (void)out; (void)max;
    return 0;   // no handles were opened on the game's behalf by a launcher: nothing to pop
}
