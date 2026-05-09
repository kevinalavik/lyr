#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define USERS_FILE "/etc/passwd"
#define ISSUE_FILE "/etc/issue"
#define MOTD_FILE "/etc/motd"

#define MAX_LINE 512
#define MAX_FIELD 128

/*
 * Supported /etc/passwd format:
 *
 *   username:x:uid:gid:gecos:home:shell:password
 *
 * Examples:
 *
 *   root:x:0:0:root:/root:/bin/sh:toor
 *   lyr:x:1000:1000:lyr:/home/lyr:/bin/sh:lyr
 *   guest:x:1001:1001:guest:/home/guest:/bin/sh:
 *
 * Empty password field means passwordless login.
 * This login manager supports plaintext passwords only.
 */

struct user_entry {
	char username[MAX_FIELD];
	uid_t uid;
	gid_t gid;
	char gecos[MAX_FIELD];
	char home[MAX_FIELD];
	char shell[MAX_FIELD];
	char password[MAX_FIELD];
	int has_password;
};

static void clear_screen(void)
{
	const char *seq = "\033[2J\033[3J\033[H";
	write(STDOUT_FILENO, seq, strlen(seq));
}

static void print_file(const char *path)
{
	FILE *fp = fopen(path, "r");

	if (!fp) {
		return;
	}

	char buf[256];

	for (;;) {
		size_t n = fread(buf, 1, sizeof(buf), fp);

		if (n > 0) {
			write(STDOUT_FILENO, buf, n);
		}

		if (n < sizeof(buf)) {
			break;
		}
	}

	fclose(fp);
}

static void print_banner(void)
{
	if (access(ISSUE_FILE, R_OK) == 0) {
		print_file(ISSUE_FILE);
		return;
	}

	const char *banner =
		"\n"
		"lyrOS 0.1.0\n"
		"Copyright (c) 2026 Kevin Alavik. All rights reserved.\n"
		"\n";

	write(STDOUT_FILENO, banner, strlen(banner));
}

static void print_motd(void)
{
	if (access(MOTD_FILE, R_OK) == 0) {
		print_file(MOTD_FILE);
		write(STDOUT_FILENO, "\n", 1);
	}
}

static void trim_newline(char *s)
{
	size_t n = strlen(s);

	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
		s[n - 1] = '\0';
		n--;
	}
}

static void trim_spaces(char *s)
{
	size_t start = 0;

	while (s[start] == ' ' || s[start] == '\t') {
		start++;
	}

	if (start > 0) {
		memmove(s, s + start, strlen(s + start) + 1);
	}

	size_t n = strlen(s);

	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
		s[n - 1] = '\0';
		n--;
	}
}

static void clear_string(char *s)
{
	if (!s) {
		return;
	}

	volatile char *p = s;

	while (*p) {
		*p++ = 0;
	}
}

static int read_line(const char *prompt, char *buf, size_t size)
{
	if (prompt) {
		write(STDOUT_FILENO, prompt, strlen(prompt));
	}

	size_t pos = 0;

	while (pos + 1 < size) {
		char c;
		ssize_t n = read(STDIN_FILENO, &c, 1);

		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}

			return -1;
		}

		if (n == 0) {
			break;
		}

		if (c == '\n' || c == '\r') {
			break;
		}

		if (c == '\b' || c == 127) {
			if (pos > 0) {
				pos--;
			}

			continue;
		}

		buf[pos++] = c;
	}

	buf[pos] = '\0';
	return 0;
}

static void redraw_password_prompt(const char *prompt, size_t stars)
{
	write(STDOUT_FILENO, "\r", 1);
	if (prompt) {
		write(STDOUT_FILENO, prompt, strlen(prompt));
	}
	for (size_t i = 0; i < stars; i++) {
		write(STDOUT_FILENO, "*", 1);
	}
	write(STDOUT_FILENO, "\033[K", 3);
}

static int read_password(const char *prompt, char *buf, size_t size)
{
	struct termios oldt;
	struct termios newt;
	int have_termios = 0;

	if (prompt) {
		write(STDOUT_FILENO, prompt, strlen(prompt));
	}

	if (tcgetattr(STDIN_FILENO, &oldt) == 0) {
		newt = oldt;
		newt.c_lflag &= ~(ECHO | ICANON);
		newt.c_cc[VMIN] = 1;
		newt.c_cc[VTIME] = 0;

		if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &newt) == 0) {
			have_termios = 1;
		}
	}

	size_t pos = 0;
	size_t visible = 0;

	while (pos + 1 < size) {
		char c;
		ssize_t n = read(STDIN_FILENO, &c, 1);

		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}

			if (have_termios) {
				tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
			}

			return -1;
		}

		if (n == 0) {
			continue;
		}

		if (c == '\n' || c == '\r') {
			break;
		}

		if (c == '\b' || c == 8 || c == 127) {
			if (pos > 0) {
				pos--;
				visible--;
				buf[pos] = '\0';
				redraw_password_prompt(prompt, visible);
			}
			continue;
		}

		if (c == 27) {
			char seq[4];
			ssize_t n1 = read(STDIN_FILENO, &seq[0], 1);
			ssize_t n2 = read(STDIN_FILENO, &seq[1], 1);

			if (n1 == 1 && n2 == 1) {
				if (seq[0] == '[' && seq[1] == '3') {
					read(STDIN_FILENO, &seq[2], 1);

					if (pos > 0) {
						pos--;
						visible--;
						buf[pos] = '\0';

						redraw_password_prompt(prompt, visible);
					}
				}
			}

			continue;
		}

		if ((unsigned char)c < 32) {
			continue;
		}

		buf[pos++] = c;
		buf[pos] = '\0';
		visible++;

		write(STDOUT_FILENO, "*", 1);
	}

	buf[pos] = '\0';

	if (have_termios) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
	}

	write(STDOUT_FILENO, "\n", 1);
	return 0;
}

static int parse_number(const char *s, unsigned long *out)
{
	if (!s || !*s) {
		return -1;
	}

	char *end = NULL;
	errno = 0;

	unsigned long value = strtoul(s, &end, 10);

	if (errno != 0 || end == s || *end != '\0') {
		return -1;
	}

	*out = value;
	return 0;
}

static int split_user_line(char *line, struct user_entry *out)
{
	char *fields[8];
	char *p = line;

	for (int i = 0; i < 8; i++) {
		fields[i] = p;

		if (i < 7) {
			char *colon = strchr(p, ':');

			if (!colon) {
				return -1;
			}

			*colon = '\0';
			p = colon + 1;
		}
	}

	if (!fields[0][0]) {
		return -1;
	}

	unsigned long uid_num;
	unsigned long gid_num;

	if (parse_number(fields[2], &uid_num) < 0) {
		return -1;
	}

	if (parse_number(fields[3], &gid_num) < 0) {
		return -1;
	}

	memset(out, 0, sizeof(*out));

	snprintf(out->username, sizeof(out->username), "%s", fields[0]);
	out->uid = (uid_t)uid_num;
	out->gid = (gid_t)gid_num;
	snprintf(out->gecos, sizeof(out->gecos), "%s", fields[4]);
	snprintf(out->home, sizeof(out->home), "%s", fields[5]);
	snprintf(out->shell, sizeof(out->shell), "%s", fields[6]);
	snprintf(out->password, sizeof(out->password), "%s", fields[7]);

	trim_newline(out->password);

	out->has_password = out->password[0] != '\0';

	if (out->home[0] == '\0') {
		snprintf(out->home, sizeof(out->home), "/");
	}

	if (out->shell[0] == '\0') {
		snprintf(out->shell, sizeof(out->shell), "/bin/sh");
	}

	return 0;
}

static int lookup_user(const char *username, struct user_entry *out)
{
	FILE *fp = fopen(USERS_FILE, "r");

	if (!fp) {
		perror(USERS_FILE);
		return -1;
	}

	char line[MAX_LINE];

	while (fgets(line, sizeof(line), fp)) {
		trim_newline(line);

		if (line[0] == '\0' || line[0] == '#') {
			continue;
		}

		struct user_entry ent;

		if (split_user_line(line, &ent) < 0) {
			continue;
		}

		if (strcmp(ent.username, username) == 0) {
			*out = ent;
			fclose(fp);
			return 0;
		}
	}

	fclose(fp);
	return -1;
}

static int check_password(const struct user_entry *user, const char *password)
{
	if (!user->has_password) {
		return 1;
	}

	return strcmp(user->password, password) == 0;
}

static void exec_user_shell(const struct user_entry *user)
{
	if (setgid(user->gid) < 0) {
		perror("setgid");
		_exit(1);
	}

	if (setuid(user->uid) < 0) {
		perror("setuid");
		_exit(1);
	}

	if (chdir(user->home) < 0) {
		perror("chdir");
		chdir("/");
	}

	char uid_buf[32];
	char gid_buf[32];

	snprintf(uid_buf, sizeof(uid_buf), "UID=%lu", (unsigned long)user->uid);
	snprintf(gid_buf, sizeof(gid_buf), "GID=%lu", (unsigned long)user->gid);

	char home_env[MAX_FIELD + 8];
	char user_env[MAX_FIELD + 8];
	char shell_env[MAX_FIELD + 8];

	snprintf(home_env, sizeof(home_env), "HOME=%s", user->home);
	snprintf(user_env, sizeof(user_env), "USER=%s", user->username);
	snprintf(shell_env, sizeof(shell_env), "SHELL=%s", user->shell);

	char *shell_argv[] = { (char *)user->shell, NULL };

	char *shell_envp[] = { user_env,  home_env,
						   shell_env, uid_buf,
						   gid_buf,	  "PATH=/bin:/sbin:/usr/bin:/usr/sbin",
						   NULL };

	execve(user->shell, shell_argv, shell_envp);

	perror("execve");
	_exit(1);
}

static int run_session(const struct user_entry *user)
{
	pid_t pid = fork();

	if (pid < 0) {
		perror("fork");
		return -1;
	}

	if (pid == 0) {
		exec_user_shell(user);
		_exit(1);
	}

	for (;;) {
		int status = 0;
		pid_t waited = waitpid(pid, &status, 0);

		if (waited < 0) {
			if (errno == EINTR) {
				continue;
			}

			perror("waitpid");
			return -1;
		}

		break;
	}

	return 0;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	for (;;) {
		clear_screen();
		print_banner();

		char username[MAX_FIELD];
		char password[MAX_FIELD];

		memset(username, 0, sizeof(username));
		memset(password, 0, sizeof(password));

		if (read_line("login: ", username, sizeof(username)) < 0) {
			write(STDERR_FILENO, "login: failed to read username\n", 31);
			return 1;
		}

		trim_spaces(username);

		if (username[0] == '\0') {
			continue;
		}

		struct user_entry user;

		if (lookup_user(username, &user) < 0) {
			write(STDOUT_FILENO, "\nlogin incorrect\n", 17);
			continue;
		}

		if (user.has_password) {
			if (read_password("password: ", password, sizeof(password)) < 0) {
				write(STDERR_FILENO, "login: failed to read password\n", 31);
				return 1;
			}

			trim_newline(password);
		} else {
			password[0] = '\0';
		}

		if (!check_password(&user, password)) {
			write(STDOUT_FILENO, "\nlogin incorrect\n", 17);
			clear_string(password);
			continue;
		}

		clear_string(password);

		write(STDOUT_FILENO, "\n", 1);

		print_motd();

		run_session(&user);
	}
}