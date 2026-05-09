#ifndef USERS_H
#define USERS_H

#include <login.h>

int users_lookup(const char *username, user_entry_t *out);
int users_check_password(const user_entry_t *user, const char *password);

#endif /* USERS_H */
