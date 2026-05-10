#ifndef LOGGING_H
#define LOGGING_H

#include <login.h>

int login_logging_setup(void);
void login_print_lastlog(const user_entry_t *user);
void login_record_success(const user_entry_t *user);
void login_record_failure(const char *username);

#endif /* LOGGING_H */
