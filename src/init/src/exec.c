#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include <lyr/mount.h>

#include "script.h"

int check_ifaces(void);

static char *xstrdup(const char *s)
{
	size_t len;
	char *out;

	len = strlen(s) + 1;
	out = malloc(len);
	if (!out)
		return NULL;

	memcpy(out, s, len);
	return out;
}

void runtime_init(struct runtime *rt)
{
	memset(rt, 0, sizeof(*rt));
}

void runtime_destroy(struct runtime *rt)
{
	for (size_t i = 0; i < rt->env.count; i++) {
		free(rt->env.vars[i]);
		rt->env.vars[i] = NULL;
	}

	rt->env.count = 0;
}

static int env_set(struct env *env, const char *assignment)
{
	const char *eq;
	size_t name_len;

	eq = strchr(assignment, '=');
	if (!eq || eq == assignment) {
		fprintf(stderr, "init: set usage: set NAME=value\n");
		return -1;
	}

	name_len = (size_t)(eq - assignment);

	for (size_t i = 0; i < env->count; i++) {
		if (strncmp(env->vars[i], assignment, name_len) == 0 &&
			env->vars[i][name_len] == '=') {
			char *copy = xstrdup(assignment);
			if (!copy)
				return -1;

			free(env->vars[i]);
			env->vars[i] = copy;
			return 0;
		}
	}

	if (env->count >= MAX_ENV) {
		fprintf(stderr, "init: too many environment variables\n");
		return -1;
	}

	env->vars[env->count] = xstrdup(assignment);
	if (!env->vars[env->count])
		return -1;

	env->count++;
	return 0;
}

static char **envp_make(struct env *env)
{
	char **envp;

	envp = calloc(env->count + 1, sizeof(char *));
	if (!envp)
		return NULL;

	for (size_t i = 0; i < env->count; i++)
		envp[i] = env->vars[i];

	envp[env->count] = NULL;
	return envp;
}

static int mount_or_report(const char *source, const char *target,
						   const char *filesystem, unsigned long flags,
						   const void *data)
{
	if (mount(source, target, filesystem, flags, data) < 0) {
		fprintf(stderr, "init: failed to mount %s at %s as %s: %s\n", source,
				target, filesystem, strerror(errno));
		return -1;
	}

	printf("init: mounted %s at %s\n", filesystem, target);
	return 0;
}

static int run_program(struct runtime *rt, char **argv)
{
	pid_t pid;
	int status;
	char **envp;

	envp = envp_make(&rt->env);
	if (!envp) {
		fprintf(stderr, "init: out of memory\n");
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "init: fork failed: %s\n", strerror(errno));
		free(envp);
		return -1;
	}

	if (pid == 0) {
		execve(argv[0], argv, envp);

		fprintf(stderr, "init: execve(%s) failed: %s\n", argv[0],
				strerror(errno));
		_exit(127);
	}

	free(envp);

	if (waitpid(pid, &status, 0) < 0) {
		fprintf(stderr, "init: waitpid failed: %s\n", strerror(errno));
		return -1;
	}

	return status;
}

static int exec_program(struct runtime *rt, char **argv)
{
	char **envp;

	envp = envp_make(&rt->env);
	if (!envp) {
		fprintf(stderr, "init: out of memory\n");
		return -1;
	}

	printf("init: executing %s\n", argv[0]);

	execve(argv[0], argv, envp);

	fprintf(stderr, "init: execve(%s) failed: %s\n", argv[0], strerror(errno));

	free(envp);
	return -1;
}

static int execute_include(struct runtime *rt, const char *path)
{
	struct script script;
	int ret;

	if (rt->include_depth >= MAX_INCLUDE) {
		fprintf(stderr, "init: include depth exceeded while opening %s\n",
				path);
		return -1;
	}

	script_init(&script);

	rt->include_depth++;

	ret = parse_file(path, &script);
	if (ret == 0)
		ret = execute_script(rt, &script);

	rt->include_depth--;

	script_destroy(&script);
	return ret;
}

static int execute_command(struct runtime *rt, const struct command *cmd)
{
	char **argv = (char **)cmd->argv;

	if (cmd->argc == 0)
		return 0;

#define ANSI_RESET "\033[0m"
#define ANSI_BOLD "\033[1m"
#define ANSI_DIM "\033[2m"
#define ANSI_CYAN "\033[36m"
#define ANSI_WHITE "\033[37m"
	if (strcmp(argv[0], "echo") == 0) {
		printf(ANSI_DIM "[" ANSI_RESET ANSI_BOLD ANSI_CYAN
						"initrc" ANSI_RESET ANSI_DIM "]" ANSI_RESET ANSI_DIM
						" :: " ANSI_RESET);

		for (int i = 1; i < cmd->argc; i++) {
			if (i > 1)
				putchar(' ');

			fputs(argv[i], stdout);
		}

		putchar('\n');
		return 0;
	}

	if (strcmp(argv[0], "set") == 0) {
		if (cmd->argc != 2) {
			fprintf(stderr, "init: line %d: set usage: set NAME=value\n",
					cmd->line);
			return -1;
		}

		return env_set(&rt->env, argv[1]);
	}

	if (strcmp(argv[0], "mount") == 0) {
		if (cmd->argc != 4) {
			fprintf(
				stderr,
				"init: line %d: mount usage: mount <source> <target> <filesystem>\n",
				cmd->line);
			return -1;
		}

		return mount_or_report(argv[1], argv[2], argv[3], 0, NULL);
	}

	if (strcmp(argv[0], "include") == 0) {
		if (cmd->argc != 2) {
			fprintf(stderr, "init: line %d: include usage: include <file>\n",
					cmd->line);
			return -1;
		}

		return execute_include(rt, argv[1]);
	}

	if (strcmp(argv[0], "ifaces") == 0) {
		return check_ifaces();
	}

	if (strcmp(argv[0], "run") == 0) {
		if (cmd->argc < 2) {
			fprintf(stderr,
					"init: line %d: run usage: run <program> [args...]\n",
					cmd->line);
			return -1;
		}

		return run_program(rt, &argv[1]);
	}

	if (strcmp(argv[0], "exec") == 0) {
		if (cmd->argc < 2) {
			fprintf(stderr,
					"init: line %d: exec usage: exec <program> [args...]\n",
					cmd->line);
			return -1;
		}

		return exec_program(rt, &argv[1]);
	}

	fprintf(stderr, "init: line %d: unknown command: %s\n", cmd->line, argv[0]);

	return -1;
}

int execute_script(struct runtime *rt, const struct script *script)
{
	int ret = 0;

	for (size_t i = 0; i < script->count; i++) {
		if (execute_command(rt, &script->commands[i]) < 0)
			ret = -1;
	}

	return ret;
}
