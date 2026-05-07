#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "script.h"

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

static void command_destroy(struct command *cmd)
{
	for (int i = 0; i < cmd->argc; i++) {
		free(cmd->argv[i]);
		cmd->argv[i] = NULL;
	}

	cmd->argc = 0;
}

void script_init(struct script *script)
{
	script->commands = NULL;
	script->count = 0;
	script->capacity = 0;
}

void script_destroy(struct script *script)
{
	for (size_t i = 0; i < script->count; i++)
		command_destroy(&script->commands[i]);

	free(script->commands);

	script->commands = NULL;
	script->count = 0;
	script->capacity = 0;
}

static int script_add_command(struct script *script, struct command *cmd)
{
	struct command *new_commands;
	size_t new_capacity;

	if (cmd->argc == 0)
		return 0;

	if (script->count == script->capacity) {
		new_capacity = script->capacity ? script->capacity * 2 : 16;

		new_commands = realloc(script->commands,
				       new_capacity * sizeof(*new_commands));
		if (!new_commands)
			return -1;

		script->commands = new_commands;
		script->capacity = new_capacity;
	}

	script->commands[script->count++] = *cmd;
	memset(cmd, 0, sizeof(*cmd));

	return 0;
}

static int command_add_arg(struct command *cmd, const char *arg)
{
	if (cmd->argc >= MAX_ARGS - 1)
		return -1;

	cmd->argv[cmd->argc] = xstrdup(arg);
	if (!cmd->argv[cmd->argc])
		return -1;

	cmd->argc++;
	cmd->argv[cmd->argc] = NULL;

	return 0;
}

static char *read_entire_file(const char *path)
{
	FILE *fp;
	char *buf;
	long size;
	size_t got;

	fp = fopen(path, "rb");
	if (!fp)
		return NULL;

	if (fseek(fp, 0, SEEK_END) < 0) {
		fclose(fp);
		return NULL;
	}

	size = ftell(fp);
	if (size < 0) {
		fclose(fp);
		return NULL;
	}

	if (fseek(fp, 0, SEEK_SET) < 0) {
		fclose(fp);
		return NULL;
	}

	buf = malloc((size_t)size + 1);
	if (!buf) {
		fclose(fp);
		return NULL;
	}

	got = fread(buf, 1, (size_t)size, fp);
	fclose(fp);

	if (got != (size_t)size) {
		free(buf);
		return NULL;
	}

	buf[got] = '\0';
	return buf;
}

static int parse_source(const char *path, const char *src,
			struct script *script)
{
	struct lexer lx;
	struct command current;

	memset(&current, 0, sizeof(current));

	lexer_init(&lx, path, src);

	for (;;) {
		struct token tok = lexer_next(&lx);

		switch (tok.type) {
		case TOK_EOF:
			if (script_add_command(script, &current) < 0) {
				command_destroy(&current);
				fprintf(stderr, "init: out of memory\n");
				return -1;
			}

			return 0;

		case TOK_ERROR:
			command_destroy(&current);
			fprintf(stderr, "init: parse error in %s:%d:%d: %s\n",
				path, tok.line, tok.column, tok.text);
			return -1;

		case TOK_NEWLINE:
		case TOK_SEMICOLON:
			if (script_add_command(script, &current) < 0) {
				command_destroy(&current);
				fprintf(stderr, "init: out of memory\n");
				return -1;
			}
			break;

		case TOK_WORD:
			if (current.argc == 0)
				current.line = tok.line;

			if (command_add_arg(&current, tok.text) < 0) {
				command_destroy(&current);
				fprintf(stderr,
					"init: too many args or out of memory at %s:%d\n",
					path, tok.line);
				return -1;
			}
			break;
		}
	}
}

int parse_file(const char *path, struct script *script)
{
	char *src;
	int ret;

	src = read_entire_file(path);
	if (!src) {
		fprintf(stderr, "init: failed to read %s: %s\n",
			path, strerror(errno));
		return -1;
	}

	ret = parse_source(path, src, script);

	free(src);
	return ret;
}
