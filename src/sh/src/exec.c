#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sh.h>
#include <sys/wait.h>
#include <lyr/input.h>

static void set_status_env(int status)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%d", status);
	setenv("?", buf, 1);
}

static int apply_assignments(sh_command_t *cmd, size_t *first_word)
{
	size_t i = 0;
	for (; i < cmd->words.n; i++) {
		char *w = cmd->words.v[i];
		if (!sh_is_assignment(w))
			break;
		char *eq = strchr(w, '=');
		char *name = sh_strndup(w, (size_t)(eq - w));
		setenv(name, eq + 1, 1);
		free(name);
	}
	*first_word = i;
	return 0;
}

static int apply_assignments_range(sh_command_t *cmd, size_t start, size_t end,
								   size_t *first_word)
{
	size_t i = start;
	for (; i < end; i++) {
		char *w = cmd->words.v[i];
		if (!sh_is_assignment(w))
			break;
		char *eq = strchr(w, '=');
		char *name = sh_strndup(w, (size_t)(eq - w));
		setenv(name, eq + 1, 1);
		free(name);
	}
	*first_word = i;
	return 0;
}

static char **argv_from_words(sh_command_t *cmd, size_t first)
{
	size_t n = cmd->words.n - first;
	char **argv = sh_xmalloc((n + 1) * sizeof(argv[0]));
	for (size_t i = 0; i < n; i++)
		argv[i] = cmd->words.v[first + i];
	argv[n] = NULL;
	return argv;
}

static char **argv_from_range(sh_command_t *cmd, size_t first, size_t end)
{
	size_t n = end - first;
	char **argv = sh_xmalloc((n + 1) * sizeof(argv[0]));
	for (size_t i = 0; i < n; i++)
		argv[i] = cmd->words.v[first + i];
	argv[n] = NULL;
	return argv;
}

static int parse_pipeline(sh_command_t *cmd, size_t *bounds,
						  size_t *stage_count)
{
	size_t stage = 0;
	size_t start = 0;

	for (size_t i = 0; i <= cmd->words.n; i++) {
		int at_end = i == cmd->words.n;
		if (!at_end && strcmp(cmd->words.v[i], "|") != 0)
			continue;

		if (i == start)
			return -1;

		bounds[stage * 2] = start;
		bounds[stage * 2 + 1] = i;
		stage++;
		start = i + 1;
	}

	if (start != cmd->words.n + 1)
		return -1;

	*stage_count = stage;
	return 0;
}

static void reset_child_signals(void)
{
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
#ifdef SIGPIPE
	signal(SIGPIPE, SIG_DFL);
#endif
}

static int run_command_argv(char **argv)
{
	char *path = sh_find_in_path(argv[0]);
	if (!path) {
		fprintf(stderr, "%s: command not found\n", argv[0]);
		return 127;
	}

	execve(path, argv, environ);
	fprintf(stderr, "%s: %s\n", path, strerror(errno));
	free(path);
	return 126;
}

static int run_external(sh_shell_t *sh, char **argv)
{
	int interactive = sh && sh->interactive && isatty(STDIN_FILENO);
	pid_t shell_pgrp = interactive ? getpgrp() : 0;
	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "fork: %s\n", strerror(errno));
		return 126;
	}

	if (pid == 0) {
		(void)setpgid(0, 0);
		if (interactive)
			(void)tcsetpgrp(STDIN_FILENO, getpid());
		reset_child_signals();
		_exit(run_command_argv(argv));
	}

	(void)setpgid(pid, pid);
	if (interactive)
		(void)tcsetpgrp(STDIN_FILENO, pid);

	int status = 0;
	pid_t waited;
	do {
		waited = waitpid(pid, &status, 0);
	} while (waited < 0 && (errno == EINTR || errno == EAGAIN));

	if (waited < 0) {
		fprintf(stderr, "waitpid: %s\n", strerror(errno));
		return 126;
	}

	if (WIFEXITED(status))
		status = WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		status = 128 + WTERMSIG(status);
	else
		status = 126;

	if (interactive) {
		lyr_kbd_t kbd;
		if (lyr_kbd_open(&kbd) == 0) {
			lyr_kbd_flush(&kbd);
			lyr_kbd_close(&kbd);
		}
	}

	if (interactive && shell_pgrp > 0)
		(void)tcsetpgrp(STDIN_FILENO, shell_pgrp);

	return status;
}

static int run_pipeline_stage(sh_shell_t *sh, sh_command_t *cmd, size_t start,
							  size_t end, int in_fd, int out_fd, pid_t pgid,
							  int interactive)
{
	size_t first = start;
	char **argv;
	int handled = 0;
	int status;

	apply_assignments_range(cmd, start, end, &first);
	if (first >= end)
		_exit(0);

	argv = argv_from_range(cmd, first, end);

	if (pgid == 0)
		pgid = getpid();
	(void)setpgid(0, pgid);
	if (interactive)
		(void)tcsetpgrp(STDIN_FILENO, pgid);

	reset_child_signals();

	if (in_fd != STDIN_FILENO) {
		if (dup2(in_fd, STDIN_FILENO) < 0) {
			fprintf(stderr, "dup2: %s\n", strerror(errno));
			free(argv);
			_exit(126);
		}
		close(in_fd);
	}

	if (out_fd != STDOUT_FILENO) {
		if (dup2(out_fd, STDOUT_FILENO) < 0) {
			fprintf(stderr, "dup2: %s\n", strerror(errno));
			free(argv);
			_exit(126);
		}
		close(out_fd);
	}

	status = sh_builtin_run(sh, (int)(end - first), argv, &handled);
	if (!handled)
		status = run_command_argv(argv);

	free(argv);
	_exit(status);
}

static int run_pipeline(sh_shell_t *sh, sh_command_t *cmd)
{
	size_t bounds[64];
	size_t stage_count = 0;
	pid_t pids[32];
	pid_t pgid = 0;
	pid_t shell_pgrp = 0;
	int interactive = sh->interactive && isatty(STDIN_FILENO);
	int prev_read = -1;
	int pipefd[2] = { -1, -1 };
	int last_status = 0;

	if (cmd->words.n == 0)
		return 0;
	if (cmd->words.n > 128)
		return 2;
	if (parse_pipeline(cmd, bounds, &stage_count) < 0 || stage_count == 0 ||
		stage_count > 32) {
		fprintf(stderr, "sh: syntax error near unexpected pipe\n");
		return 2;
	}

	if (interactive)
		shell_pgrp = getpgrp();

	for (size_t i = 0; i < stage_count; i++) {
		int in_fd = prev_read >= 0 ? prev_read : STDIN_FILENO;
		int out_fd = STDOUT_FILENO;

		if (i + 1 < stage_count) {
			if (pipe(pipefd) < 0) {
				fprintf(stderr, "pipe: %s\n", strerror(errno));
				if (prev_read >= 0)
					close(prev_read);
				return 126;
			}
			out_fd = pipefd[1];
		} else {
			pipefd[0] = -1;
			pipefd[1] = -1;
		}

		pid_t pid = fork();
		if (pid < 0) {
			fprintf(stderr, "fork: %s\n", strerror(errno));
			if (prev_read >= 0)
				close(prev_read);
			if (pipefd[0] >= 0)
				close(pipefd[0]);
			if (pipefd[1] >= 0)
				close(pipefd[1]);
			return 126;
		}

		if (pid == 0)
			run_pipeline_stage(sh, cmd, bounds[i * 2], bounds[i * 2 + 1], in_fd,
							   out_fd, pgid, interactive);

		if (pgid == 0)
			pgid = pid;
		(void)setpgid(pid, pgid);
		pids[i] = pid;

		if (prev_read >= 0)
			close(prev_read);
		if (pipefd[1] >= 0)
			close(pipefd[1]);
		prev_read = pipefd[0];
	}

	if (prev_read >= 0)
		close(prev_read);

	if (interactive && pgid > 0)
		(void)tcsetpgrp(STDIN_FILENO, pgid);

	for (size_t i = 0; i < stage_count; i++) {
		int status = 0;
		pid_t waited;
		do {
			waited = waitpid(pids[i], &status, 0);
		} while (waited < 0 && (errno == EINTR || errno == EAGAIN));

		if (waited < 0) {
			fprintf(stderr, "waitpid: %s\n", strerror(errno));
			last_status = 126;
			continue;
		}

		if (WIFEXITED(status))
			last_status = WEXITSTATUS(status);
		else if (WIFSIGNALED(status))
			last_status = 128 + WTERMSIG(status);
		else
			last_status = 126;
	}

	if (interactive && shell_pgrp > 0)
		(void)tcsetpgrp(STDIN_FILENO, shell_pgrp);

	return last_status;
}

int sh_execute(sh_shell_t *sh, sh_command_t *cmd)
{
	if (cmd->words.n == 0)
		return 0;

	int expand_status = sh_expand_globs(cmd);
	if (expand_status != 0) {
		sh->last_status = 1;
		set_status_env(1);
		return 1;
	}

	for (size_t i = 0; i < cmd->words.n; i++) {
		if (strcmp(cmd->words.v[i], "|") == 0) {
			int status = run_pipeline(sh, cmd);
			sh->last_status = status;
			set_status_env(status);
			return status;
		}
	}

	size_t first = 0;
	apply_assignments(cmd, &first);
	if (first >= cmd->words.n) {
		set_status_env(0);
		return 0;
	}

	char **argv = argv_from_words(cmd, first);
	int handled = 0;
	int status =
		sh_builtin_run(sh, (int)(cmd->words.n - first), argv, &handled);
	if (!handled)
		status = run_external(sh, argv);
	free(argv);

	sh->last_status = status;
	set_status_env(status);
	return status;
}

int sh_run_line(sh_shell_t *sh, const char *line)
{
	sh_command_list_t list;
	char *err = NULL;
	if (sh_parse_line(line, &list, &err) != 0) {
		fprintf(stderr, "sh: syntax error: %s\n", err ? err : "parse failed");
		free(err);
		sh->last_status = 2;
		set_status_env(2);
		return 2;
	}

	int status = 0;
	for (size_t i = 0; i < list.n; i++) {
		status = sh_execute(sh, &list.v[i]);
		if (sh->should_exit)
			break;
	}
	sh_command_list_free(&list);
	return status;
}

int sh_run_file(sh_shell_t *sh, const char *path)
{
	FILE *fp = fopen(path, "r");
	if (!fp) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return 1;
	}

	int old_interactive = sh->interactive;
	char *old_script = sh->script_name;
	sh->interactive = 0;
	sh->script_name = sh_xstrdup(path);

	int status = 0;
	char *line;
	while (!sh->should_exit && (line = sh_read_line(fp)) != NULL) {
		status = sh_run_line(sh, line);
		free(line);
	}

	free(sh->script_name);
	sh->script_name = old_script;
	sh->interactive = old_interactive;
	fclose(fp);
	return status;
}
