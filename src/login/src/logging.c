#include <logging.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define AUTHLOG_FILE "/var/log/login.log"
#define LOG_DIR "/var/log"
#define VAR_DIR "/var"

static void write_stdout(const char *s)
{
	if (!s)
		return;
	write(STDOUT_FILENO, s, strlen(s));
}

static const char *current_tty(void)
{
	const char *tty = ttyname(STDIN_FILENO);

	if (!tty)
		tty = ttyname(STDOUT_FILENO);

	if (!tty)
		return "tty?";

	if (strncmp(tty, "/dev/", 5) == 0)
		tty += 5;

	return tty;
}

static int ensure_dir(const char *path, mode_t mode)
{
	struct stat st;

	if (stat(path, &st) == 0) {
		if (S_ISDIR(st.st_mode))
			return 0;
		errno = ENOTDIR;
		return -1;
	}

	if (mkdir(path, mode) == 0)
		return 0;

	if (errno == EEXIST)
		return 0;

	return -1;
}

int login_logging_setup(void)
{
	int rc = 0;

	if (ensure_dir(VAR_DIR, 0755) < 0)
		rc = -1;
	if (ensure_dir(LOG_DIR, 0755) < 0)
		rc = -1;

	return rc;
}

static void format_time(time_t t, char *buf, size_t size)
{
	struct tm tm;

	if (t == (time_t)-1 || !localtime_r(&t, &tm) ||
		strftime(buf, size, "%a %b %e %H:%M:%S %Y", &tm) == 0) {
		snprintf(buf, size, "unknown time");
	}
}

static void append_authlog(const char *event, const char *username)
{
	login_logging_setup();

	FILE *fp = fopen(AUTHLOG_FILE, "a");
	if (!fp)
		return;

	time_t now = time(NULL);
	char tbuf[64];
	format_time(now, tbuf, sizeof(tbuf));

	fprintf(fp, "%s %s user=%s tty=%s\n", tbuf, event,
			username && username[0] ? username : "?", current_tty());
	fclose(fp);
}

void login_record_failure(const char *username)
{
	append_authlog("failed-login", username);
}

void login_record_success(const user_entry_t *user)
{
	if (!user)
		return;

	login_logging_setup();

	time_t now = time(NULL);
	char tbuf[64];
	format_time(now, tbuf, sizeof(tbuf));

	FILE *fp = fopen(LASTLOG_FILE, "a");
	if (fp) {
		fprintf(fp, "%s:%lu:%s\n", user->username,
				(unsigned long)now, current_tty());
		fclose(fp);
	}

	append_authlog("login", user->username);
}

void login_print_lastlog(const user_entry_t *user)
{
	if (!user)
		return;

	login_logging_setup();

	FILE *fp = fopen(LASTLOG_FILE, "r");
	if (!fp) {
		write_stdout("Last login: never\n");
		return;
	}

	char line[MAX_LINE];
	time_t last_time = (time_t)-1;
	char last_tty[MAX_FIELD];
	last_tty[0] = '\0';

	while (fgets(line, sizeof(line), fp)) {
		char *user_field = line;
		char *time_field = strchr(user_field, ':');
		if (!time_field)
			continue;
		*time_field++ = '\0';

		char *tty_field = strchr(time_field, ':');
		if (!tty_field)
			continue;
		*tty_field++ = '\0';

		char *nl = strchr(tty_field, '\n');
		if (nl)
			*nl = '\0';

		if (strcmp(user_field, user->username) != 0)
			continue;

		char *end = NULL;
		errno = 0;
		unsigned long parsed = strtoul(time_field, &end, 10);
		if (errno || end == time_field || *end != '\0')
			continue;

		last_time = (time_t)parsed;
		snprintf(last_tty, sizeof(last_tty), "%s", tty_field);
	}

	fclose(fp);

	if (last_time == (time_t)-1) {
		write_stdout("Last login: never\n");
		return;
	}

	char tbuf[64];
	char out[256];
	format_time(last_time, tbuf, sizeof(tbuf));
	snprintf(out, sizeof(out), "Last login: %s on %s\n", tbuf,
			 last_tty[0] ? last_tty : "tty?");
	write_stdout(out);
}
