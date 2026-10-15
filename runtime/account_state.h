#pragma once
#include <stdint.h>
// One local, offline user. Uid is generated once per install and persisted (like a console user id).
typedef struct { uint64_t v[2]; } wonder_uid;           // nn::account::Uid (16 bytes, all-zero = invalid)
typedef struct { uint64_t v[3]; } wonder_user_handle;   // nn::account::UserHandle (24 bytes)
int wonder_account_initialize(const char *root);        // 0 or errno
int wonder_account_ready(void);
wonder_uid wonder_account_uid(void);
int wonder_account_is_user(const wonder_uid *uid);
const char *wonder_account_nickname(void);              // UTF-8, <= 32 bytes
int wonder_account_open(wonder_user_handle *out, const wonder_uid *uid);   // 0 ok, -1 unknown user
int wonder_account_close(const wonder_user_handle *handle);                // 0 ok, -1 not an open handle
int wonder_account_handle_uid(const wonder_user_handle *handle, wonder_uid *out);
int wonder_account_pop_open(wonder_user_handle *out, int max);             // moves the preselected-user handle out
