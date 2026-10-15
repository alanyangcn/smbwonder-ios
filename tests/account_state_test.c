#include "account_state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
int main(int argc, char **argv) {
    assert(argc == 2); const char *root = argv[1];
    assert(!wonder_account_ready() && wonder_account_initialize(NULL));
    assert(!wonder_account_initialize(root) && wonder_account_ready());
    wonder_uid u = wonder_account_uid(); assert(u.v[0] || u.v[1]);
    assert(!strcmp(wonder_account_nickname(), "Player"));
    wonder_uid other = {{u.v[0] ^ 1, u.v[1]}};
    assert(wonder_account_is_user(&u) && !wonder_account_is_user(&other));
    wonder_user_handle h, bad;
    assert(wonder_account_open(&h, &other) == -1 && !wonder_account_open(&h, &u));
    wonder_uid back; assert(!wonder_account_handle_uid(&h, &back) && back.v[0] == u.v[0] && back.v[1] == u.v[1]);
    bad = h; bad.v[2] ^= 1; assert(wonder_account_handle_uid(&bad, &back) == -1 && wonder_account_close(&bad) == -1);
    assert(!wonder_account_close(&h) && wonder_account_close(&h) == -1);   // not open any more
    assert(wonder_account_pop_open(&h, 1) == 0);
    // Persistence: the same uid is read back by a fresh process (argv[1]/account/uid.bin).
    char path[512]; snprintf(path, sizeof path, "%s/account/uid.bin", root);
    FILE *f = fopen(path, "rb"); wonder_uid stored; assert(f && fread(&stored, 1, 16, f) == 16); fclose(f);
    assert(!memcmp(&stored, &u, 16));
    printf("ACCOUNT_UID %016llx%016llx\n", (unsigned long long)u.v[1], (unsigned long long)u.v[0]);
    puts("ACCOUNT_STATE_OK");
}
