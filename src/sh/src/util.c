#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sh.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

void *sh_xmalloc(size_t size)
{
	void *p = malloc(size);
	if (!p) {
		fprintf(stderr, "sh: out of memory\n");
		exit(2);
	}
	return p;
}

void *sh_xrealloc(void *ptr, size_t size)
{
	void *p = realloc(ptr, size);
	if (!p) {
		fprintf(stderr, "sh: out of memory\n");
		exit(2);
	}
	return p;
}

char *sh_xstrdup(const char *s)
{
	if (!s)
		s = "";
	char *p = strdup(s);
	if (!p) {
		fprintf(stderr, "sh: out of memory\n");
		exit(2);
	}
	return p;
}

char *sh_strndup(const char *s, size_t len)
{
	char *p = sh_xmalloc(len + 1);
	memcpy(p, s, len);
	p[len] = '\0';
	return p;
}

int sh_is_name(const char *s)
{
	if (!s || !*s)
		return 0;
	if (!(isalpha((unsigned char)*s) || *s == '_'))
		return 0;
	s++;
	while (*s) {
		if (!(isalnum((unsigned char)*s) || *s == '_'))
			return 0;
		s++;
	}
	return 1;
}

int sh_is_assignment(const char *s)
{
	const char *eq = strchr(s, '=');
	if (!eq || eq == s)
		return 0;
	char *name = sh_strndup(s, (size_t)(eq - s));
	int ok = sh_is_name(name);
	free(name);
	return ok;
}

char *sh_getcwd_alloc(void)
{
	char tmp[PATH_MAX];
	if (!getcwd(tmp, sizeof(tmp)))
		return sh_xstrdup("?");
	return sh_xstrdup(tmp);
}

char *sh_path_join(const char *a, const char *b)
{
	size_t alen = strlen(a);
	size_t blen = strlen(b);
	int slash = alen && a[alen - 1] != '/';
	char *out = sh_xmalloc(alen + (size_t)slash + blen + 1);
	memcpy(out, a, alen);
	if (slash)
		out[alen++] = '/';
	memcpy(out + alen, b, blen + 1);
	return out;
}

char *sh_find_in_path(const char *name)
{
	if (!name || !*name)
		return NULL;
	if (strchr(name, '/'))
		return access(name, X_OK) == 0 ? sh_xstrdup(name) : NULL;

	const char *path = getenv("PATH");
	if (!path || !*path)
		path = "/usr/bin:/bin";

	const char *p = path;
	while (1) {
		const char *colon = strchr(p, ':');
		size_t len = colon ? (size_t)(colon - p) : strlen(p);
		char *dir = len ? sh_strndup(p, len) : sh_xstrdup(".");
		char *full = sh_path_join(dir, name);
		free(dir);
		if (access(full, X_OK) == 0)
			return full;
		free(full);
		if (!colon)
			break;
		p = colon + 1;
	}
	return NULL;
}

void sh_wordv_init(sh_wordv_t *wv)
{
	wv->v = NULL;
	wv->n = 0;
	wv->cap = 0;
}

void sh_wordv_push(sh_wordv_t *wv, char *word)
{
	if (wv->n + 1 >= wv->cap) {
		wv->cap = wv->cap ? wv->cap * 2 : 8;
		wv->v = sh_xrealloc(wv->v, wv->cap * sizeof(wv->v[0]));
	}
	wv->v[wv->n++] = word;
	wv->v[wv->n] = NULL;
}

void sh_wordv_free(sh_wordv_t *wv)
{
	for (size_t i = 0; i < wv->n; i++)
		free(wv->v[i]);
	free(wv->v);
	wv->v = NULL;
	wv->n = 0;
	wv->cap = 0;
}

void sh_command_list_init(sh_command_list_t *cl)
{
	cl->v = NULL;
	cl->n = 0;
	cl->cap = 0;
}

void sh_command_list_push(sh_command_list_t *cl, sh_command_t cmd)
{
	if (cl->n >= cl->cap) {
		cl->cap = cl->cap ? cl->cap * 2 : 4;
		cl->v = sh_xrealloc(cl->v, cl->cap * sizeof(cl->v[0]));
	}
	cl->v[cl->n++] = cmd;
}

void sh_command_list_free(sh_command_list_t *cl)
{
	for (size_t i = 0; i < cl->n; i++)
		sh_wordv_free(&cl->v[i].words);
	free(cl->v);
	cl->v = NULL;
	cl->n = 0;
	cl->cap = 0;
}

char *sh_read_line(FILE *fp)
{
	size_t cap = 128;
	size_t len = 0;
	char *buf = sh_xmalloc(cap);

	for (;;) {
		int c = fgetc(fp);
		if (c == EOF) {
			if (ferror(fp) && errno == EINTR) {
				clearerr(fp);
				free(buf);
				errno = EINTR;
				return NULL;
			}
			if (len == 0) {
				free(buf);
				errno = 0;
				return NULL;
			}
			break;
		}
		if (len + 1 >= cap) {
			cap *= 2;
			buf = sh_xrealloc(buf, cap);
		}
		buf[len++] = (char)c;
		if (c == '\n')
			break;
	}
	buf[len] = '\0';
	return buf;
}

void sh_restore_terminal(void)
{
	struct termios tio;

	if (!isatty(STDIN_FILENO))
		return;
	if (tcgetattr(STDIN_FILENO, &tio) < 0)
		return;

	tio.c_lflag |= (tcflag_t)(ECHO | ICANON | ISIG | IEXTEN);
	tio.c_cc[VINTR] = 3;
	tio.c_cc[VQUIT] = 28;
	tio.c_cc[VERASE] = 127;
	tio.c_cc[VKILL] = 21;
	tio.c_cc[VEOF] = 4;
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;

	(void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &tio);
}

void sh_history_add(sh_shell_t *sh, const char *line)
{
	if (!line || !*line)
		return;
	if (sh->history_count >= sh->history_cap) {
		sh->history_cap = sh->history_cap ? sh->history_cap * 2 : 32;
		sh->history =
			sh_xrealloc(sh->history, sh->history_cap * sizeof(sh->history[0]));
	}
	sh->history[sh->history_count++] = sh_xstrdup(line);
}

void sh_history_free(sh_shell_t *sh)
{
	for (size_t i = 0; i < sh->history_count; i++)
		free(sh->history[i]);
	free(sh->history);
	sh->history = NULL;
	sh->history_count = 0;
	sh->history_cap = 0;
}
