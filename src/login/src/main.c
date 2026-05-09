#include <login.h>
#include <banner.h>
#include <session.h>
#include <term.h>
#include <users.h>

#include <string.h>
#include <unistd.h>

int main(void)
{
	for (;;) {
		banner_print_issue();
		char username[MAX_FIELD];
		char password[MAX_FIELD];

		memset(username, 0, sizeof(username));
		if (term_read_line("login: ", username,
						   sizeof(username)) < 0) {
			write(STDERR_FILENO, "login: failed to read username\n", 31);
			return 1;
		}
		term_trim_spaces(username);
		if (!username[0])
			continue;

		user_entry_t user;
		if (users_lookup(username, &user) < 0) {
			memset(password, 0, sizeof(password));
			term_read_password("Password: ", password,
							   sizeof(password));
			term_clear_string(password);
			term_println(NULL, "Login incorrect");
			sleep(LOCKOUT_SECS);
			continue;
		}

		int tries = 0;
		int authenticated = 0;

		while (tries < MAX_LOGIN_TRIES) {
			memset(password, 0, sizeof(password));

			if (user.has_password) {
				if (term_read_password("Password: ",
									   password, sizeof(password)) < 0) {
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
			term_println(NULL,
						 "Login incorrect");
			sleep(LOCKOUT_SECS);
			continue;
		}

		banner_print_motd();
		session_run(&user);
	}
}
