#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sh.h>
#include <builtin.h>

extern char **environ;

static int is_name(const char *s)
{
	if (!s || !*s)
		return 0;

	if (s[0] >= '0' && s[0] <= '9')
		return 0;

	for (size_t i = 0; s[i]; i++) {
		char c = s[i];
		if (c >= 'a' && c <= 'z')
			continue;
		if (c >= 'A' && c <= 'Z')
			continue;
		if (c >= '0' && c <= '9')
			continue;
		if (c == '_')
			continue;
		return 0;
	}

	return 1;
}

int sh_builtin_export(int argc, char **argv)
{
	if (argc == 1) {
		for (char **env = environ; env && *env; env++)
			printf("export %s\n", *env);
		return 0;
	}

	int status = 0;

	for (int i = 1; i < argc; i++) {
		char *eq = strchr(argv[i], '=');

		if (!eq) {
			if (!is_name(argv[i])) {
				fprintf(stderr, "export: bad name: %s\n", argv[i]);
				status = 1;
			}
			continue;
		}

		char *name = sh_strndup(argv[i], (size_t)(eq - argv[i]));

		if (!is_name(name)) {
			fprintf(stderr, "export: bad name: %s\n", name);
			free(name);
			status = 1;
			continue;
		}

		if (setenv(name, eq + 1, 1) != 0) {
			fprintf(stderr, "export: %s: %s\n", name, strerror(errno));
			status = 1;
		}

		free(name);
	}

	return status;
}

int sh_builtin_unset(int argc, char **argv)
{
	int status = 0;

	for (int i = 1; i < argc; i++) {
		if (!is_name(argv[i])) {
			fprintf(stderr, "unset: bad variable name: %s\n", argv[i]);
			status = 1;
			continue;
		}

		unsetenv(argv[i]);
	}

	return status;
}

int sh_builtin_env(void)
{
	for (char **env = environ; env && *env; env++)
		puts(*env);

	return 0;
}

int sh_builtin_set(int argc, char **argv)
{
	if (argc == 1)
		return sh_builtin_env();

	int status = 0;

	for (int i = 1; i < argc; i++) {
		if (!sh_is_assignment(argv[i])) {
			fprintf(stderr, "set: expected NAME=value: %s\n", argv[i]);
			status = 1;
			continue;
		}

		char *eq = strchr(argv[i], '=');
		char *name = sh_strndup(argv[i], (size_t)(eq - argv[i]));

		if (setenv(name, eq + 1, 1) != 0) {
			fprintf(stderr, "set: %s: %s\n", name, strerror(errno));
			status = 1;
		}

		free(name);
	}

	return status;
}