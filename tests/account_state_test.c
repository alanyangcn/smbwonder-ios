// argv: <root> <mode>. mode "fresh": a new install gets 玩家1..玩家8; "legacy": an existing uid.bin + nickname.txt
// becomes the first user; "check": a second process reads back what the first one persisted.
#include "account_state.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
static void print_users(void) {
    wonder_uid list[WONDER_ACCOUNT_MAX_USERS]; int n = wonder_account_list(list, WONDER_ACCOUNT_MAX_USERS);
    for (int i = 0; i < n; i++) { char hex[33], name[33]; wonder_uid_hex(&list[i], hex); assert(!wonder_account_nickname(&list[i], name)); printf("USER %s %s\n", hex, name); }
}
int main(int argc, char **argv) {
    assert(argc == 3); const char *root = argv[1], *mode = argv[2];
    assert(!wonder_account_ready() && wonder_account_initialize(NULL));
    assert(wonder_account_list(NULL, 0) == 0);
    if (!strcmp(mode, "legacy")) {
        char dir[512], path[600]; snprintf(dir, sizeof dir, "%s/account", root); mkdir(dir, 0700);
        wonder_uid old = {{0x1111, 0x2222}};
        snprintf(path, sizeof path, "%s/uid.bin", dir); FILE *f = fopen(path, "wb"); fwrite(&old, 1, 16, f); fclose(f);
        snprintf(path, sizeof path, "%s/nickname.txt", dir); f = fopen(path, "w"); fputs("玩家3\n", f); fclose(f);
        assert(!wonder_account_initialize(root));
        wonder_uid list[8]; assert(wonder_account_list(list, 8) == 8);
        assert(list[0].v[0] == 0x1111 && list[0].v[1] == 0x2222);   // the old user (and its save) comes first
        char name[33]; assert(!wonder_account_nickname(&list[0], name) && !strcmp(name, "玩家3"));
        assert(!wonder_account_nickname(&list[1], name) && !strcmp(name, "玩家1"));
        assert(!wonder_account_nickname(&list[2], name) && !strcmp(name, "玩家2"));
        assert(!wonder_account_nickname(&list[3], name) && !strcmp(name, "玩家4"));
        print_users(); puts("ACCOUNT_LEGACY_OK"); return 0;
    }
    assert(!wonder_account_initialize(root) && wonder_account_ready());
    if (!strcmp(mode, "check")) { print_users(); puts("ACCOUNT_CHECK_OK"); return 0; }
    wonder_uid list[10]; assert(wonder_account_list(list, 10) == 8 && wonder_account_list(list, 3) == 8);
    for (int i = 0; i < 8; i++) {
        char name[33], want[33]; snprintf(want, sizeof want, "玩家%d", i + 1);
        assert(!wonder_account_nickname(&list[i], name) && !strcmp(name, want));
        assert(wonder_account_is_user(&list[i]));
        for (int k = 0; k < i; k++) assert(memcmp(&list[i], &list[k], 16));
    }
    char hex[33]; wonder_uid back; wonder_uid_hex(&list[0], hex); assert(!wonder_uid_parse(&back, hex) && !memcmp(&back, &list[0], 16));
    assert(wonder_uid_parse(&back, "00000000000000000000000000000000") && wonder_uid_parse(&back, "zz"));
    wonder_uid other = {{list[0].v[0] ^ 1, list[0].v[1]}};
    assert(!wonder_account_is_user(&other));
    // Handles.
    wonder_user_handle h, bad;
    assert(wonder_account_open(&h, &other) == -1 && !wonder_account_open(&h, &list[2]));
    assert(!wonder_account_handle_uid(&h, &back) && !memcmp(&back, &list[2], 16));
    assert(wonder_account_open_count(&list[2]) == 1);
    bad = h; bad.v[2] ^= 1; assert(wonder_account_handle_uid(&bad, &back) == -1 && wonder_account_close(&bad) == -1);
    assert(wonder_account_remove(&list[2]) == EBUSY);   // held open by the game
    assert(!wonder_account_close(&h) && wonder_account_close(&h) == -1);
    assert(wonder_account_pop_open(&h, 1) == 0);
    // Management: full at 8, remove frees a slot, the new user takes the lowest free 玩家N.
    assert(wonder_account_add(NULL) == ENOSPC);
    assert(!wonder_account_remove(&list[2]) && !wonder_account_is_user(&list[2]) && wonder_account_list(NULL, 0) == 7);
    wonder_uid added; assert(!wonder_account_add(&added)); char name[33];
    assert(!wonder_account_nickname(&added, name) && !strcmp(name, "玩家3"));
    wonder_uid now[8]; assert(wonder_account_list(now, 8) == 8 && !memcmp(&now[7], &added, 16));   // appended
    assert(wonder_account_rename(&added, "") == EINVAL && wonder_account_rename(&added, "a\nb") == EINVAL);
    assert(wonder_account_rename(&added, "123456789012345678901234567890123") == EINVAL);   // 33 bytes
    assert(wonder_account_rename(&other, "x") == ENOENT);
    assert(!wonder_account_rename(&added, "马力欧 Mario"));
    assert(!wonder_account_nickname(&added, name) && !strcmp(name, "马力欧 Mario"));
    for (int i = 0; i < 7; i++) assert(!wonder_account_remove(&now[i]));
    assert(wonder_account_remove(&added) == EPERM && wonder_account_list(NULL, 0) == 1);   // the last user stays
    assert(!wonder_account_add(NULL));
    print_users(); puts("ACCOUNT_STATE_OK");
}
