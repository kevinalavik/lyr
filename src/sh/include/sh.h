#ifndef LYR_SH_H
#define LYR_SH_H

#include <stddef.h>
#include <stdio.h>

extern char **environ;

typedef struct sh_shell {
	int interactive;
	int should_exit;
	int last_status;
	char *script_name;
	char **history;
	size_t history_count;
	size_t history_cap;
} sh_shell_t;

typedef struct sh_wordv {
	char **v;
	size_t n;
	size_t cap;
} sh_wordv_t;

typedef struct sh_command {
	sh_wordv_t words;
} sh_command_t;

typedef struct sh_command_list {
	sh_command_t *v;
	size_t n;
	size_t cap;
} sh_command_list_t;

void *sh_xmalloc(size_t size);
void *sh_xrealloc(void *ptr, size_t size);
char *sh_xstrdup(const char *s);
char *sh_strndup(const char *s, size_t len);
int sh_is_name(const char *s);
int sh_is_assignment(const char *s);
char *sh_getcwd_alloc(void);
char *sh_path_join(const char *a, const char *b);
char *sh_find_in_path(const char *name);

void sh_wordv_init(sh_wordv_t *wv);
void sh_wordv_push(sh_wordv_t *wv, char *word);
void sh_wordv_free(sh_wordv_t *wv);

void sh_command_list_init(sh_command_list_t *cl);
void sh_command_list_push(sh_command_list_t *cl, sh_command_t cmd);
void sh_command_list_free(sh_command_list_t *cl);

char *sh_read_line(FILE *fp);
char *sh_expand_vars(const char *input);
char *sh_expand_prompt(const char *ps1);
int sh_parse_line(const char *line, sh_command_list_t *out, char **err);

int sh_builtin_run(sh_shell_t *sh, int argc, char **argv, int *handled);
int sh_execute(sh_shell_t *sh, sh_command_t *cmd);
int sh_run_line(sh_shell_t *sh, const char *line);
int sh_run_file(sh_shell_t *sh, const char *path);
void sh_history_add(sh_shell_t *sh, const char *line);
void sh_history_free(sh_shell_t *sh);

#endif