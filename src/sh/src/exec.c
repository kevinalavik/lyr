#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sh.h>
#include <sys/wait.h>

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

static char **argv_from_words(sh_command_t *cmd, size_t first)
{
	size_t n = cmd->words.n - first;
	char **argv = sh_xmalloc((n + 1) * sizeof(argv[0]));
	for (size_t i = 0; i < n; i++)
		argv[i] = cmd->words.v[first + i];
	argv[n] = NULL;
	return argv;
}

static int run_external(char **argv)
{
	char *path = sh_find_in_path(argv[0]);
	if (!path) {
		fprintf(stderr, "%s: command not found\n", argv[0]);
		return 127;
	}

	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "fork: %s\n", strerror(errno));
		free(path);
		return 126;
	}

	if (pid == 0) {
		execve(path, argv, environ);
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		_exit(126);
	}

	free(path);

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
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);

	return 126;
}

int sh_execute(sh_shell_t *sh, sh_command_t *cmd)
{
	if (cmd->words.n == 0)
		return 0;

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
		status = run_external(argv);
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