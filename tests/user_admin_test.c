// argv: <root> <phase>. "queue": first launch, saves exist, deletions are queued but nothing is touched yet;
// "apply": the next launch carries them out before anything else.
#include "user_admin.h"
#include "savedata.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
_Noreturn void wonder_save_fatal(const char *format, ...) { va_list ap; va_start(ap, format); vfprintf(stderr, format, ap); va_end(ap); abort(); }
int main(int argc, char **argv) {
    assert(argc == 3); const char *root = argv[1], *phase = argv[2];
    assert(!wonder_users_startup(root));
    wonder_uid u[8]; int n = wonder_account_list(u, 8);
    char pending[600]; snprintf(pending, sizeof pending, "%s/account/pending.txt", root);
    struct stat st;
    if (!strcmp(phase, "queue")) {
        assert(n == 8);
        for (int i = 0; i < 3; i++) assert(!wonder_save_ensure(u[i].v) && wonder_users_has_save(&u[i]));
        assert(!wonder_users_has_save(&u[3]));
        assert(!wonder_users_schedule(&u[0], WONDER_PENDING_SAVE));
        assert(!wonder_users_schedule(&u[1], WONDER_PENDING_USER));
        assert(wonder_users_pending(&u[1]) == (WONDER_PENDING_USER | WONDER_PENDING_SAVE));   // a user goes with its save
        assert(!wonder_users_schedule(&u[2], WONDER_PENDING_SAVE) && !wonder_users_cancel(&u[2]) && !wonder_users_pending(&u[2]));
        wonder_uid stranger = {{1, 2}}; assert(wonder_users_schedule(&stranger, WONDER_PENDING_SAVE) == ENOENT);
        // Queued only: users and saves are untouched while the game may be using them.
        assert(wonder_account_list(NULL, 0) == 8 && wonder_users_has_save(&u[0]) && wonder_users_has_save(&u[1]));
        assert(!stat(pending, &st));
        // At least one user always stays.
        for (int i = 2; i < 8; i++) assert(!wonder_users_schedule(&u[i], WONDER_PENDING_USER));
        assert(wonder_users_schedule(&u[0], WONDER_PENDING_USER) == EPERM && wonder_users_pending(&u[0]) == WONDER_PENDING_SAVE);
        for (int i = 3; i < 8; i++) assert(!wonder_users_cancel(&u[i]));   // keep only 0 (save), 1, 2 (users)
        puts("USER_ADMIN_QUEUE_OK"); return 0;
    }
    // Applied at startup: user 1 and 2 are gone (with their saves), user 0 keeps its account but lost its save.
    assert(n == 6 && stat(pending, &st) && errno == ENOENT);
    assert(!wonder_users_has_save(&u[0]) && !wonder_users_pending(&u[0]));
    char dir[700]; snprintf(dir, sizeof dir, "%s/save", root);
    int dirs = 0; char cmd[800]; snprintf(cmd, sizeof cmd, "ls %s | wc -l", dir); FILE *p = popen(cmd, "r"); assert(fscanf(p, "%d", &dirs) == 1); pclose(p);
    assert(dirs == 0);
    puts("USER_ADMIN_APPLY_OK");
}
