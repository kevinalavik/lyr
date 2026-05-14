#include <login.h>
#include <banner.h>
#include <logging.h>
#include <session.h>
#include <term.h>
#include <users.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <lyr/input.h>
#include <errno.h>

#define KBD_CONFIG_FILE "/etc/kbd"

static int load_default_keyboard_layout(void)
{
	int fd;
	char buf[64] = { 0 };
	ssize_t n;
	char path[LYR_KBD_MAP_PATH_MAX];
	lyr_kbd_t kbd;

	fd = open(KBD_CONFIG_FILE, O_RDONLY);
	if (fd < 0)
		return -1;

	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;

	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
		buf[--n] = '\0';
	}
	if (n == 0)
		return -1;

	snprintf(path, sizeof(path), "/share/kbd/%s", buf);

	if (lyr_kbd_open(&kbd) < 0)
		return -1;

	if (lyr_kbd_set_layout(&kbd, path) < 0) {
		lyr_kbd_close(&kbd);
		return -1;
	}

	lyr_kbd_close(&kbd);
	return 0;
}

static int bind_tty(const char *path)
{
	if (!path || !path[0])
		return 0;

	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	if (open(path, O_RDONLY) != STDIN_FILENO)
		return -1;
	if (open(path, O_WRONLY) != STDOUT_FILENO)
		return -1;
	if (open(path, O_WRONLY) != STDERR_FILENO)
		return -1;

	return 0;
}

static int handle_session(const user_entry_t *user)
{
	login_print_lastlog(user);
	login_record_success(user);
	banner_print_motd();
	return session_run(user);
}

int main(int argc, char **argv)
{
	const char *tty_path = NULL;
	const char *autologin_user = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--autologin") == 0) {
			if (i + 1 >= argc) {
				write(STDERR_FILENO,
					  "usage: login [TTY] [--autologin USER]\n", 39);
				return 1;
			}
			autologin_user = argv[++i];
			continue;
		}

		if (!tty_path) {
			tty_path = argv[i];
			continue;
		}

		write(STDERR_FILENO, "usage: login [TTY] [--autologin USER]\n", 39);
		return 1;
	}

	if (tty_path && bind_tty(tty_path) < 0) {
		perror("login: bind_tty");
		return 1;
	}

	login_logging_setup();

	load_default_keyboard_layout();

	for (;;) {
		banner_print_issue();

		if (autologin_user && autologin_user[0]) {
			user_entry_t user;
			if (users_lookup(autologin_user, &user) < 0) {
				dprintf(STDERR_FILENO,
						"login: autologin user '%s' not found\n",
						autologin_user);
				return 1;
			}

			term_println(NULL, "");
			dprintf(STDOUT_FILENO, "Autologin: %s\n", user.username);
			handle_session(&user);
			/* Avoid a fork/exec storm if the shell exits immediately. */
			sleep(1);
			continue;
		}

		char username[MAX_FIELD];
		char password[MAX_FIELD];

		memset(username, 0, sizeof(username));
		if (term_read_line("login: ", username, sizeof(username)) < 0) {
			write(STDERR_FILENO, "login: failed to read username\n", 31);
			return 1;
		}
		term_trim_spaces(username);
		if (!username[0])
			continue;

		user_entry_t user;
		if (users_lookup(username, &user) < 0) {
			memset(password, 0, sizeof(password));
			term_read_password("Password: ", password, sizeof(password));
			term_clear_string(password);
			login_record_failure(username);
			term_println(NULL, "Login incorrect");
			sleep(LOCKOUT_SECS);
			continue;
		}

		int tries = 0;
		int authenticated = 0;

		while (tries < MAX_LOGIN_TRIES) {
			memset(password, 0, sizeof(password));

			if (user.has_password) {
				if (term_read_password("Password: ", password,
									   sizeof(password)) < 0) {
					write(STDERR_FILENO, "login: failed to read password\n",
						  31);
					return 1;
				}
				term_trim_newline(password);
			}

			if (users_check_password(&user, password)) {
				term_clear_string(password);
				authenticated = 1;
				break;
			}

			term_clear_string(password);
			tries++;

			if (tries < MAX_LOGIN_TRIES) {
				term_println(NULL, "Login incorrect");
				sleep(1);
			}
		}

		if (!authenticated) {
			login_record_failure(username);
			term_println(NULL, "Login incorrect");
			sleep(LOCKOUT_SECS);
			continue;
		}

		handle_session(&user);
	}
}
