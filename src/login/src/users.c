#include <users.h>
#include <term.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_ulong(const char *s, unsigned long *out)
{
	if (!s || !*s)
		return -1;
	char *end = NULL;
	errno = 0;
	*out = strtoul(s, &end, 10);
	if (errno || end == s || *end != '\0')
		return -1;
	return 0;
}

static int parse_passwd_line(char *line, user_entry_t *out)
{
	char *fields[8];
	char *p = line;

	for (int i = 0; i < 8; i++) {
		fields[i] = p;
		if (i < 7) {
			char *colon = strchr(p, ':');
			if (!colon)
				return -1;
			*colon = '\0';
			p = colon + 1;
		}
	}

	if (!fields[0][0])
		return -1;

	unsigned long uid_val, gid_val;
	if (parse_ulong(fields[2], &uid_val) < 0)
		return -1;
	if (parse_ulong(fields[3], &gid_val) < 0)
		return -1;

	memset(out, 0, sizeof(*out));

	snprintf(out->username, sizeof(out->username), "%s", fields[0]);
	out->uid = (uid_t)uid_val;
	out->gid = (gid_t)gid_val;
	snprintf(out->gecos, sizeof(out->gecos), "%s", fields[4]);
	snprintf(out->home, sizeof(out->home), "%s", fields[5]);
	snprintf(out->shell, sizeof(out->shell), "%s", fields[6]);
	snprintf(out->password, sizeof(out->password), "%s", fields[7]);

	term_trim_newline(out->password);

	out->has_password = out->password[0] != '\0';

	if (!out->home[0])
		snprintf(out->home, sizeof(out->home), "/");
	if (!out->shell[0])
		snprintf(out->shell, sizeof(out->shell), "/bin/sh");

	return 0;
}

int users_lookup(const char *username, user_entry_t *out)
{
	FILE *fp = fopen(USERS_FILE, "r");
	if (!fp) {
		perror(USERS_FILE);
		return -1;
	}

	char line[MAX_LINE];
	int found = 0;

	while (fgets(line, sizeof(line), fp)) {
		term_trim_newline(line);
		if (!line[0] || line[0] == '#')
			continue;

		user_entry_t ent;
		if (parse_passwd_line(line, &ent) < 0)
			continue;

		if (strcmp(ent.username, username) == 0) {
			*out = ent;
			found = 1;
			break;
		}
	}

	fclose(fp);
	return found ? 0 : -1;
}

int users_check_password(const user_entry_t *user, const char *password)
{
	if (!user->has_password)
		return 1;
	return strcmp(user->password, password) == 0;
}
