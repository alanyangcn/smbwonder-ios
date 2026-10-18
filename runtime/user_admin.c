// Queued save/user deletions (user_admin.h). pending.txt: one "<uid hex> <flags>" line per user, rewritten atomically.
#include "user_admin.h"
#include "savedata.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char pending_path[PATH_MAX];
static struct { wonder_uid uid; int flags; } queue[WONDER_ACCOUNT_MAX_USERS];
static int queued;
static int same(const wonder_uid *a, const wonder_uid *b) { return a->v[0] == b->v[0] && a->v[1] == b->v[1]; }
static int slot(const wonder_uid *uid) { for (int i = 0; i < queued; i++) if (same(&queue[i].uid, uid)) return i; return -1; }
static int write_queue(void) {
    if (!queued) return unlink(pending_path) && errno != ENOENT ? errno : 0;
    char tmp[PATH_MAX + 8]; snprintf(tmp, sizeof tmp, "%s.tmp", pending_path);
    FILE *f = fopen(tmp, "w"); if (!f) return errno;
    for (int i = 0; i < queued; i++) { char hex[33]; wonder_uid_hex(&queue[i].uid, hex); fprintf(f, "%s %d\n", hex, queue[i].flags); }
    int error = fflush(f) || fsync(fileno(f)) ? errno : 0;
    if (fclose(f) && !error) error = errno;
    if (!error && rename(tmp, pending_path)) error = errno;
    return error;
}
static void apply(const wonder_uid *uid, int flags) {
    char hex[33]; wonder_uid_hex(uid, hex);
    if (!wonder_account_is_user(uid)) { fprintf(stderr, "HOST_USERS pending %s: no such user, dropped\n", hex); return; }
    uint32_t r = wonder_save_remove(uid->v);
    fprintf(stderr, "HOST_USERS deleted save %s (%s)\n", hex, r ? "none" : "removed");
    if (flags & WONDER_PENDING_USER) {
        int e = wonder_account_remove(uid);
        fprintf(stderr, "HOST_USERS deleted user %s error=%d\n", hex, e);
    }
}
int wonder_users_startup(const char *root) {
    int error = wonder_account_initialize(root);
    if (!error) error = wonder_save_configure(root);
    if (error) return error;
    pthread_mutex_lock(&lock);
    snprintf(pending_path, sizeof pending_path, "%s/account/pending.txt", root);
    queued = 0;
    FILE *f = fopen(pending_path, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            wonder_uid uid; int flags = atoi(line + (strlen(line) > 33 ? 33 : 0));
            if (strlen(line) < 34 || line[32] != ' ' || wonder_uid_parse(&uid, line) || flags <= 0) continue;
            apply(&uid, flags);
        }
        fclose(f);
    }
    error = write_queue();   // the queue is done (or was malformed): remove it
    pthread_mutex_unlock(&lock);
    return error;
}
int wonder_users_pending(const wonder_uid *uid) {
    pthread_mutex_lock(&lock); int i = slot(uid), flags = i < 0 ? 0 : queue[i].flags; pthread_mutex_unlock(&lock); return flags;
}
int wonder_users_schedule(const wonder_uid *uid, int flags) {
    if (!wonder_account_is_user(uid)) return ENOENT;
    flags &= WONDER_PENDING_SAVE | WONDER_PENDING_USER;
    if (flags & WONDER_PENDING_USER) flags |= WONDER_PENDING_SAVE;
    pthread_mutex_lock(&lock);
    int error = 0, i = slot(uid);
    if (flags & WONDER_PENDING_USER) {   // at least one user must stay
        int users = wonder_account_list(NULL, 0), leaving = 0;
        for (int k = 0; k < queued; k++) leaving += k != i && (queue[k].flags & WONDER_PENDING_USER);
        if (users - leaving <= 1) error = EPERM;
    }
    if (!error) {
        int before = i < 0 ? 0 : queue[i].flags;
        if (i < 0) { i = queued++; queue[i].uid = *uid; }
        queue[i].flags |= flags;
        if ((error = write_queue())) { if (before) queue[i].flags = before; else queued--; }
    }
    pthread_mutex_unlock(&lock);
    return error;
}
int wonder_users_cancel(const wonder_uid *uid) {
    pthread_mutex_lock(&lock);
    int error = 0, i = slot(uid);
    if (i >= 0) {
        __typeof__(queue[0]) saved = queue[i];
        queue[i] = queue[--queued];
        if ((error = write_queue())) { queue[queued++] = queue[i]; queue[i] = saved; }
    }
    pthread_mutex_unlock(&lock);
    return error;
}
int wonder_users_has_save(const wonder_uid *uid) {
    int exists = 0; return wonder_save_exists(&exists, uid->v) ? 0 : exists;
}
