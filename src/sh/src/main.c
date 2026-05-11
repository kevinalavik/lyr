#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sh.h>

#define SYSTEM_RC "/etc/shellrc"

static void init_defaults(void)
{
	if (!getenv("PATH"))
		setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
	if (!getenv("PS1"))
		setenv("PS1", "\\u@\\h:\\w \\$ ", 1);
	if (!getenv("USER"))
		setenv("USER", "root", 1);
	if (!getenv("HOME"))
		setenv("HOME", "/root", 1);
	setenv("?", "0", 1);
}

static void usage(FILE *fp)
{
	fprintf(fp, "lyr sh v1.0 (c) 2026 Kevin Alavik\n");
	fprintf(fp, "usage: sh [-c command] [script]\n");
}

static int file_exists_readable(const char *path)
{
	return path && access(path, R_OK) == 0;
}

static void run_startup_files(sh_shell_t *sh)
{
	if (!sh->interactive)
		return;

	if (file_exists_readable(SYSTEM_RC))
		sh_run_file(sh, SYSTEM_RC);
}

int main(int argc, char **argv)
{
	init_defaults();

	sh_shell_t sh;
	memset(&sh, 0, sizeof(sh));
	sh.interactive = isatty(STDIN_FILENO);
	sh.last_status = 0;

	if (sh.interactive) {
		(void)setpgid(0, 0);
		(void)tcsetpgrp(STDIN_FILENO, getpgrp());
		signal(SIGINT, SIG_IGN);
		signal(SIGQUIT, SIG_IGN);
	}

	if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
		int status = sh_run_line(&sh, argv[2]);
		sh_history_free(&sh);
		return status;
	}

	if (argc >= 2) {
		if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
			usage(stdout);
			return 0;
		}

		int status = sh_run_file(&sh, argv[1]);
		sh_history_free(&sh);
		return status;
	}

	run_startup_files(&sh);

	while (!sh.should_exit) {
		if (sh.interactive)
			sh_restore_terminal();

		if (sh.interactive) {
			char *prompt = sh_expand_prompt(getenv("PS1"));
			fputs(prompt, stdout);
			fflush(stdout);
			free(prompt);
		}

		char *line = sh_read_line(stdin);
		if (!line) {
			if (sh.interactive && errno == EINTR) {
				errno = 0;
				continue;
			}
			if (sh.interactive)
				putchar('\n');
			break;
		}

		if (sh.interactive)
			sh_history_add(&sh, line);

		sh_run_line(&sh, line);
		if (sh.interactive)
			sh_restore_terminal();
		free(line);
	}

	int status = sh.last_status;
	sh_history_free(&sh);
	return status;
}
