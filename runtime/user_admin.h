#pragma once
#include "account_state.h"
// Host user management behind the settings panel (host_users.m). Adding and renaming users take effect at once
// (account_state.c). Deleting a save or a user is queued in <root>/account/pending.txt and carried out at the next
// launch, before the game starts: while the game runs it may hold that user open, keep the save mounted, or have it in
// memory and write it back at its next commit. Deleting a user also deletes its save, as on the console.
#define WONDER_PENDING_SAVE 1
#define WONDER_PENDING_USER 2
int wonder_users_startup(const char *root);                 // account + save roots, then apply the queue; 0 or errno
int wonder_users_pending(const wonder_uid *uid);            // WONDER_PENDING_* flags queued for the user
int wonder_users_schedule(const wonder_uid *uid, int flags); // 0, ENOENT unknown user, EPERM would leave no user
int wonder_users_cancel(const wonder_uid *uid);             // drop everything queued for the user
int wonder_users_has_save(const wonder_uid *uid);           // a committed save image exists
