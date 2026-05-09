#include <session.h>
#include <login.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

static void exec_shell(const user_entry_t *user)
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

	char uid_buf[32], gid_buf[32];
	char home_env[MAX_FIELD + 8];
	char user_env[MAX_FIELD + 8];
	char shell_env[MAX_FIELD + 8];
	char logname_env[MAX_FIELD + 12];

	snprintf(uid_buf, sizeof(uid_buf), "UID=%lu", (unsigned long)user->uid);
	snprintf(gid_buf, sizeof(gid_buf), "GID=%lu", (unsigned long)user->gid);
	snprintf(home_env, sizeof(home_env), "HOME=%s", user->home);
	snprintf(user_env, sizeof(user_env), "USER=%s", user->username);
	snprintf(logname_env, sizeof(logname_env), "LOGNAME=%s", user->username);
	snprintf(shell_env, sizeof(shell_env), "SHELL=%s", user->shell);

	char *envp[] = {
		user_env,
		logname_env,
		home_env,
		shell_env,
		uid_buf,
		gid_buf,
		"PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin",
		"TERM=lyrtern",
		NULL,
	};

	char login_name[MAX_FIELD + 2];
	snprintf(login_name, sizeof(login_name), "-%s", user->shell);

	char *argv[] = { login_name, NULL };

	execve(user->shell, argv, envp);
	perror("execve");
	_exit(1);
}

int session_run(const user_entry_t *user)
{
	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}

	if (pid == 0) {
		exec_shell(user);
		_exit(1);
	}

	for (;;) {
		int status = 0;
		pid_t waited = waitpid(pid, &status, 0);
		if (waited < 0) {
			if (errno == EINTR)
				continue;
			perror("waitpid");
			return -1;
		}
		break;
	}

	return 0;
}
