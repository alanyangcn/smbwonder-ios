#pragma once
#include <stdint.h>
// Local, offline users (at most 8, the console's limit; the game's own picker lists 8, main+0x265654). Uids are
// generated on the host and persisted (like console user ids) in <root>/account/users.txt, one "<uid hex> <nickname>"
// line per user in list order. A fresh install gets 玩家1..玩家8; an install from before multi-user support keeps its
// single uid (account/uid.bin, and its save) as the first user.
#define WONDER_ACCOUNT_MAX_USERS 8
#define WONDER_NICKNAME_MAX 32                           // bytes of UTF-8 (nn::account::Nickname is char[33])
typedef struct { uint64_t v[2]; } wonder_uid;           // nn::account::Uid (16 bytes, all-zero = invalid)
typedef struct { uint64_t v[3]; } wonder_user_handle;   // nn::account::UserHandle (24 bytes)
int wonder_account_initialize(const char *root);        // 0 or errno; later calls are no-ops
int wonder_account_ready(void);
int wonder_account_list(wonder_uid *out, int max);      // number of users; copies up to max uids in list order
int wonder_account_is_user(const wonder_uid *uid);
int wonder_account_nickname(const wonder_uid *uid, char out[WONDER_NICKNAME_MAX + 1]);   // 0 ok, -1 unknown user
int wonder_account_open(wonder_user_handle *out, const wonder_uid *uid);   // 0 ok, -1 unknown user
int wonder_account_close(const wonder_user_handle *handle);                // 0 ok, -1 not an open handle
int wonder_account_handle_uid(const wonder_user_handle *handle, wonder_uid *out);
int wonder_account_pop_open(wonder_user_handle *out, int max);             // moves the preselected-user handle out
int wonder_account_open_count(const wonder_uid *uid);                      // handles the game holds for the user
// Host user management (the settings panel). Each returns 0 or an errno and persists before returning.
int wonder_account_add(wonder_uid *out);                // ENOSPC when 8 users exist; nickname 玩家N (lowest free N)
int wonder_account_rename(const wonder_uid *uid, const char *nickname);   // EINVAL: empty, > 32 bytes or control chars
int wonder_account_remove(const wonder_uid *uid);       // ENOENT, EBUSY while open, EPERM for the last user
void wonder_uid_hex(const wonder_uid *uid, char out[33]);                  // v[1] then v[0], as save directories
int wonder_uid_parse(wonder_uid *out, const char *hex);                    // 0 ok, -1 not 32 hex digits / zero
