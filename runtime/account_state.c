// nn::account host state: up to 8 offline local users, persisted in <root>/account/users.txt (account_state.h).
// Before multi-user support the host had one user: account/uid.bin (+ optional nickname.txt, else "Player"). That
// uid becomes the first user so its save directory stays attached; uid.bin itself is left in place.
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
static int ready;
static char users_path[PATH_MAX];
static struct user { wonder_uid uid; char nickname[WONDER_NICKNAME_MAX + 1]; int open; } users[WONDER_ACCOUNT_MAX_USERS];
static int user_count;
static int random_bytes(void *p, size_t n) {
#if defined(__APPLE__)
    arc4random_buf(p, n); return 0;
#else
    int fd = open("/dev/urandom", O_RDONLY); if (fd < 0) return errno;
    ssize_t r = read(fd, p, n); close(fd); return r == (ssize_t)n ? 0 : EIO;
#endif
}
void wonder_uid_hex(const wonder_uid *uid, char out[33]) {
    snprintf(out, 33, "%016llx%016llx", (unsigned long long)uid->v[1], (unsigned long long)uid->v[0]);
}
int wonder_uid_parse(wonder_uid *out, const char *hex) {
    uint64_t v[2] = {0, 0};
    for (int i = 0; i < 32; i++) {
        char c = hex[i]; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        v[i < 16 ? 1 : 0] = v[i < 16 ? 1 : 0] << 4 | (uint64_t)d;
    }
    if (!v[0] && !v[1]) return -1;
    out->v[0] = v[0]; out->v[1] = v[1]; return 0;
}
static int valid_nickname(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > WONDER_NICKNAME_MAX) return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f) return 0;
    return 1;
}
static struct user *find(const wonder_uid *u) {
    for (int i = 0; u && i < user_count; i++) if (users[i].uid.v[0] == u->v[0] && users[i].uid.v[1] == u->v[1]) return &users[i];
    return NULL;
}
static int save_users(void) {   // atomic replace: a crash leaves the old or the new list
    char tmp[PATH_MAX + 8]; snprintf(tmp, sizeof tmp, "%s.tmp", users_path);
    FILE *f = fopen(tmp, "w"); if (!f) return errno;
    for (int i = 0; i < user_count; i++) { char hex[33]; wonder_uid_hex(&users[i].uid, hex); fprintf(f, "%s %s\n", hex, users[i].nickname); }
    int error = fflush(f) || fsync(fileno(f)) ? errno : 0;
    if (fclose(f) && !error) error = errno;
    if (!error && rename(tmp, users_path)) error = errno;
    return error;
}
static int new_uid(wonder_uid *out) {
    do { int e = random_bytes(out, sizeof *out); if (e) return e; } while ((!out->v[0] && !out->v[1]) || find(out));
    return 0;
}
static void default_nickname(char out[WONDER_NICKNAME_MAX + 1]) {   // 玩家N with the lowest N no user has
    for (int n = 1;; n++) {
        snprintf(out, WONDER_NICKNAME_MAX + 1, "玩家%d", n);
        int taken = 0; for (int i = 0; i < user_count; i++) taken |= !strcmp(users[i].nickname, out);
        if (!taken) return;
    }
}
static void append_user(const wonder_uid *uid, const char *nickname) {
    char name[WONDER_NICKNAME_MAX + 1];
    if (nickname) strcpy(name, nickname); else default_nickname(name);
    struct user *u = &users[user_count++]; memset(u, 0, sizeof *u);
    u->uid = *uid; strcpy(u->nickname, name);
}
static int load_users(FILE *f) {
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        wonder_uid uid;
        if (strlen(line) < 34 || line[32] != ' ' || wonder_uid_parse(&uid, line) || !valid_nickname(line + 33) || find(&uid) || user_count == WONDER_ACCOUNT_MAX_USERS) return EINVAL;
        append_user(&uid, line + 33);
    }
    return user_count ? 0 : EINVAL;
}
static int migrate(const char *dir) {   // no users.txt yet: keep a pre-existing uid, then fill up to 8 users
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/uid.bin", dir);
    FILE *f = fopen(path, "rb");
    if (f) {
        wonder_uid uid; size_t n = fread(&uid, 1, sizeof uid, f); fclose(f);
        if (n != sizeof uid || (!uid.v[0] && !uid.v[1])) return EINVAL;
        char nickname[64] = {0};
        snprintf(path, sizeof path, "%s/nickname.txt", dir);
        if ((f = fopen(path, "r"))) { if (!fgets(nickname, sizeof nickname, f)) nickname[0] = 0; fclose(f); nickname[strcspn(nickname, "\r\n")] = 0; }
        append_user(&uid, valid_nickname(nickname) ? nickname : NULL);
    }
    while (user_count < WONDER_ACCOUNT_MAX_USERS) { wonder_uid uid; int e = new_uid(&uid); if (e) return e; append_user(&uid, NULL); }
    return save_users();
}
int wonder_account_initialize(const char *root) {
    if (!root) return ENODEV;
    pthread_mutex_lock(&lock);
    int error = 0;
    if (ready) goto done;
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s/account", root) >= (int)sizeof dir) { error = ENAMETOOLONG; goto done; }
    if (mkdir(dir, 0700) && errno != EEXIST) { error = errno; goto done; }
    snprintf(users_path, sizeof users_path, "%s/users.txt", dir);
    user_count = 0;
    FILE *f = fopen(users_path, "r");
    if (f) { error = load_users(f); fclose(f); }
    else error = errno == ENOENT ? migrate(dir) : errno;
    if (!error) ready = 1; else user_count = 0;
done:
    pthread_mutex_unlock(&lock);
    return error;
}
int wonder_account_ready(void) { return ready; }
int wonder_account_list(wonder_uid *out, int max) {
    pthread_mutex_lock(&lock);
    int n = ready ? user_count : 0;
    for (int i = 0; out && i < n && i < max; i++) out[i] = users[i].uid;
    pthread_mutex_unlock(&lock);
    return n;
}
int wonder_account_is_user(const wonder_uid *u) {
    pthread_mutex_lock(&lock); int r = ready && find(u) != NULL; pthread_mutex_unlock(&lock); return r;
}
int wonder_account_nickname(const wonder_uid *uid, char out[WONDER_NICKNAME_MAX + 1]) {
    pthread_mutex_lock(&lock);
    struct user *u = ready ? find(uid) : NULL;
    if (u && out) strcpy(out, u->nickname);
    pthread_mutex_unlock(&lock);
    return u && out ? 0 : -1;
}
int wonder_account_open(wonder_user_handle *out, const wonder_uid *uid) {
    pthread_mutex_lock(&lock);
    struct user *u = ready && out ? find(uid) : NULL;
    if (u) { u->open++; *out = (wonder_user_handle){{u->uid.v[0], u->uid.v[1], HANDLE_MAGIC}}; }
    pthread_mutex_unlock(&lock);
    return u ? 0 : -1;
}
static struct user *handle_user(const wonder_user_handle *h) {
    if (!h || h->v[2] != HANDLE_MAGIC) return NULL;
    struct user *u = find(&(wonder_uid){{h->v[0], h->v[1]}});
    return u && u->open > 0 ? u : NULL;
}
int wonder_account_close(const wonder_user_handle *h) {
    pthread_mutex_lock(&lock);
    struct user *u = handle_user(h);
    if (u) u->open--;
    pthread_mutex_unlock(&lock);
    return u ? 0 : -1;
}
int wonder_account_handle_uid(const wonder_user_handle *h, wonder_uid *out) {
    pthread_mutex_lock(&lock);
    struct user *u = out ? handle_user(h) : NULL;
    if (u) *out = u->uid;
    pthread_mutex_unlock(&lock);
    return u ? 0 : -1;
}
int wonder_account_pop_open(wonder_user_handle *out, int max) {
    (void)out; (void)max;
    return 0;   // no handles were opened on the game's behalf by a launcher: nothing to pop
}
int wonder_account_open_count(const wonder_uid *uid) {
    pthread_mutex_lock(&lock); struct user *u = find(uid); int n = u ? u->open : 0; pthread_mutex_unlock(&lock); return n;
}
int wonder_account_add(wonder_uid *out) {
    pthread_mutex_lock(&lock);
    int error = !ready ? ENODEV : user_count == WONDER_ACCOUNT_MAX_USERS ? ENOSPC : 0;
    wonder_uid uid;
    if (!error) error = new_uid(&uid);
    if (!error) {
        append_user(&uid, NULL);
        if ((error = save_users())) user_count--;
        else if (out) *out = uid;
    }
    pthread_mutex_unlock(&lock);
    return error;
}
int wonder_account_rename(const wonder_uid *uid, const char *nickname) {
    if (!valid_nickname(nickname)) return EINVAL;
    pthread_mutex_lock(&lock);
    struct user *u = ready ? find(uid) : NULL; int error = u ? 0 : ENOENT;
    if (u) {
        char old[sizeof u->nickname]; strcpy(old, u->nickname); strcpy(u->nickname, nickname);
        if ((error = save_users())) strcpy(u->nickname, old);
    }
    pthread_mutex_unlock(&lock);
    return error;
}
int wonder_account_remove(const wonder_uid *uid) {
    pthread_mutex_lock(&lock);
    struct user *u = ready ? find(uid) : NULL;
    int error = !u ? ENOENT : u->open ? EBUSY : user_count == 1 ? EPERM : 0;
    if (!error) {
        struct user saved[WONDER_ACCOUNT_MAX_USERS]; memcpy(saved, users, sizeof users); int saved_count = user_count;
        int i = (int)(u - users); memmove(&users[i], &users[i + 1], (size_t)(user_count - i - 1) * sizeof *users); user_count--;
        if ((error = save_users())) { memcpy(users, saved, sizeof users); user_count = saved_count; }
    }
    pthread_mutex_unlock(&lock);
    return error;
}
