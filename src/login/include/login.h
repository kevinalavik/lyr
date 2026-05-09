#ifndef LOGIN_H
#define LOGIN_H

#include <sys/types.h>

#define USERS_FILE "/etc/passwd"
#define ISSUE_FILE "/etc/issue"
#define MOTD_FILE "/etc/motd"
#define LASTLOG_FILE "/var/log/lastlog"

#define MAX_LINE 512
#define MAX_FIELD 128
#define MAX_LOGIN_TRIES 3
#define LOCKOUT_SECS 5


typedef struct {
	char username[MAX_FIELD];
	uid_t uid;
	gid_t gid;
	char gecos[MAX_FIELD];
	char home[MAX_FIELD];
	char shell[MAX_FIELD];
	char password[MAX_FIELD]; /* plaintext (or empty) */
	int has_password;
} user_entry_t;

#endif /* LOGIN_H */
