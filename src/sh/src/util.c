#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

typedef struct sh_pattern {
	char *text;
	unsigned char *quoted;
	size_t len;
} sh_pattern_t;

static int pattern_piece_cmp(const void *a, const void *b)
{
	const char *const *sa = a;
	const char *const *sb = b;
	return strcmp(*sa, *sb);
}

static int sh_match_char_class(const sh_pattern_t *pat, size_t *idx, char ch)
{
	size_t i = *idx + 1;
	int negate = 0;
	int matched = 0;

	if (i < pat->len && !pat->quoted[i] &&
		(pat->text[i] == '!' || pat->text[i] == '^')) {
		negate = 1;
		i++;
	}

	for (; i < pat->len; i++) {
		if (!pat->quoted[i] && pat->text[i] == ']')
			break;

		if (i + 2 < pat->len && !pat->quoted[i] && !pat->quoted[i + 1] &&
			!pat->quoted[i + 2] && pat->text[i + 1] == '-' &&
			pat->text[i + 2] != ']') {
			char start = pat->text[i];
			char end = pat->text[i + 2];
			if (start <= ch && ch <= end)
				matched = 1;
			i += 2;
			continue;
		}

		if (pat->text[i] == ch)
			matched = 1;
	}

	if (i >= pat->len || pat->quoted[i] || pat->text[i] != ']')
		return -1;

	*idx = i;
	return negate ? !matched : matched;
}

static int sh_match_pattern_impl(const sh_pattern_t *pat, size_t pi,
								 const char *text, size_t ti)
{
	while (pi < pat->len) {
		char pc = pat->text[pi];

		if (!pat->quoted[pi] && pc == '*') {
			while (pi + 1 < pat->len && !pat->quoted[pi + 1] &&
				   pat->text[pi + 1] == '*')
				pi++;

			pi++;
			if (pi == pat->len)
				return 1;

			for (size_t k = ti;; k++) {
				if (sh_match_pattern_impl(pat, pi, text, k))
					return 1;
				if (!text[k])
					break;
			}
			return 0;
		}

		if (!text[ti])
			return 0;

		if (!pat->quoted[pi] && pc == '?') {
			pi++;
			ti++;
			continue;
		}

		if (!pat->quoted[pi] && pc == '[') {
			int r = sh_match_char_class(pat, &pi, text[ti]);
			if (r < 0) {
				if (pc != text[ti])
					return 0;
			} else if (!r) {
				return 0;
			}
			pi++;
			ti++;
			continue;
		}

		if (pc != text[ti])
			return 0;

		pi++;
		ti++;
	}

	return text[ti] == '\0';
}

static int sh_compile_pattern(const char *input, sh_pattern_t *pat)
{
	size_t cap = strlen(input) + 1;

	pat->text = sh_xmalloc(cap);
	pat->quoted = sh_xmalloc(cap);
	pat->len = 0;

	int quoted = 0;
	for (size_t i = 0; input[i]; i++) {
		if (input[i] == SH_NOGLOB_BEGIN) {
			quoted = 1;
			continue;
		}
		if (input[i] == SH_NOGLOB_END) {
			quoted = 0;
			continue;
		}
		if (input[i] == SH_NOEXPAND_BEGIN || input[i] == SH_NOEXPAND_END)
			continue;

		pat->text[pat->len] = input[i];
		pat->quoted[pat->len] = (unsigned char)quoted;
		pat->len++;
	}

	pat->text[pat->len] = '\0';
	pat->quoted[pat->len] = 0;
	return 0;
}

static void sh_free_pattern(sh_pattern_t *pat)
{
	free(pat->text);
	free(pat->quoted);
	pat->text = NULL;
	pat->quoted = NULL;
	pat->len = 0;
}

char *sh_unquote_word(const char *input)
{
	sh_pattern_t pat;
	sh_compile_pattern(input, &pat);
	char *out = sh_xstrdup(pat.text);
	sh_free_pattern(&pat);
	return out;
}

static int sh_pattern_has_meta(const sh_pattern_t *pat)
{
	for (size_t i = 0; i < pat->len; i++) {
		if (pat->quoted[i])
			continue;
		if (pat->text[i] == '*' || pat->text[i] == '?' || pat->text[i] == '[')
			return 1;
	}
	return 0;
}

static int sh_pattern_starts_dot(const sh_pattern_t *pat)
{
	return pat->len > 0 && pat->text[0] == '.';
}

static char *sh_pattern_literal(const sh_pattern_t *pat)
{
	return sh_xstrdup(pat->text);
}

static char **sh_split_pattern_segments(const char *pattern, size_t *count)
{
	sh_wordv_t parts;
	sh_wordv_init(&parts);

	size_t start = 0;
	size_t len = strlen(pattern);
	for (size_t i = 0; i <= len; i++) {
		if (pattern[i] == '/' || pattern[i] == '\0') {
			sh_wordv_push(&parts, sh_strndup(pattern + start, i - start));
			start = i + 1;
		}
	}

	*count = parts.n;
	return parts.v;
}

static char *sh_join_display_path(const char *prefix, const char *name)
{
	if (!prefix || !*prefix)
		return sh_xstrdup(name);
	if (strcmp(prefix, "/") == 0)
		return sh_path_join("/", name);
	return sh_path_join(prefix, name);
}

static char *sh_join_search_path(const char *base, const char *name)
{
	if (!base || !*base)
		return sh_xstrdup(name);
	if (strcmp(base, "/") == 0)
		return sh_path_join("/", name);
	return sh_path_join(base, name);
}

static int sh_expand_pattern_recursive(const char *search_base,
									   const char *display_base,
									   char **segments, size_t index,
									   size_t segment_count,
									   sh_wordv_t *matches)
{
	sh_pattern_t pat;
	sh_compile_pattern(segments[index], &pat);
	int has_meta = sh_pattern_has_meta(&pat);
	int need_dot = sh_pattern_starts_dot(&pat);
	int last = index + 1 == segment_count;
	int status = 0;

	if (!has_meta) {
		char *literal = sh_pattern_literal(&pat);
		char *next_search = sh_join_search_path(search_base, literal);
		char *next_display = sh_join_display_path(display_base, literal);
		sh_free_pattern(&pat);
		free(literal);

		if (last) {
			sh_wordv_push(matches, next_display);
			free(next_search);
			return 0;
		}

		struct stat st;
		if (stat(next_search, &st) == 0 && S_ISDIR(st.st_mode))
			status = sh_expand_pattern_recursive(next_search, next_display,
												 segments, index + 1,
												 segment_count, matches);
		free(next_search);
		free(next_display);
		return status;
	}

	DIR *dir = opendir(search_base);
	if (!dir) {
		sh_free_pattern(&pat);
		return 0;
	}

	sh_wordv_t names;
	sh_wordv_init(&names);
	for (;;) {
		struct dirent *ent = readdir(dir);
		if (!ent)
			break;
		if (!need_dot && ent->d_name[0] == '.')
			continue;
		if (sh_match_pattern_impl(&pat, 0, ent->d_name, 0))
			sh_wordv_push(&names, sh_xstrdup(ent->d_name));
	}
	closedir(dir);
	sh_free_pattern(&pat);

	qsort(names.v, names.n, sizeof(names.v[0]), pattern_piece_cmp);

	for (size_t i = 0; i < names.n; i++) {
		char *next_search = sh_join_search_path(search_base, names.v[i]);
		char *next_display = sh_join_display_path(display_base, names.v[i]);

		if (last) {
			sh_wordv_push(matches, next_display);
		} else {
			struct stat st;
			if (stat(next_search, &st) == 0 && S_ISDIR(st.st_mode))
				status = sh_expand_pattern_recursive(next_search, next_display,
													 segments, index + 1,
													 segment_count, matches);
			free(next_display);
		}

		free(next_search);
		if (status != 0)
			break;
	}

	sh_wordv_free(&names);
	return status;
}

static int sh_expand_single_word(const char *word, sh_wordv_t *out)
{
	sh_pattern_t pat;
	sh_compile_pattern(word, &pat);
	int has_meta = sh_pattern_has_meta(&pat);
	char *literal = sh_xstrdup(pat.text);
	sh_free_pattern(&pat);

	if (!has_meta) {
		sh_wordv_push(out, literal);
		return 0;
	}

	size_t segment_count = 0;
	char **segments = sh_split_pattern_segments(literal, &segment_count);
	sh_wordv_t matches;
	sh_wordv_init(&matches);

	int absolute = literal[0] == '/';
	const char *search_base = absolute ? "/" : ".";
	const char *display_base = absolute ? "/" : "";

	int status = sh_expand_pattern_recursive(search_base, display_base, segments,
											 0, segment_count, &matches);

	for (size_t i = 0; i < segment_count; i++)
		free(segments[i]);
	free(segments);

	if (status != 0) {
		sh_wordv_free(&matches);
		free(literal);
		return status;
	}

	if (matches.n == 0) {
		sh_wordv_push(out, literal);
		return 0;
	}

	free(literal);
	for (size_t i = 0; i < matches.n; i++)
		sh_wordv_push(out, matches.v[i]);
	free(matches.v);
	return 0;
}

int sh_expand_globs(sh_command_t *cmd)
{
	sh_wordv_t expanded;
	sh_wordv_init(&expanded);

	size_t stage_start = 0;
	for (size_t i = 0; i <= cmd->words.n; i++) {
		int at_end = i == cmd->words.n;
		int at_pipe = !at_end && strcmp(cmd->words.v[i], "|") == 0;

		if (!at_end && !at_pipe)
			continue;

		size_t first_word = stage_start;
		while (first_word < i && sh_is_assignment(cmd->words.v[first_word]))
			first_word++;

		for (size_t j = stage_start; j < i; j++) {
			if (j < first_word) {
				sh_wordv_push(&expanded, sh_unquote_word(cmd->words.v[j]));
				continue;
			}

			int r = sh_expand_single_word(cmd->words.v[j], &expanded);
			if (r != 0) {
				sh_wordv_free(&expanded);
				return r;
			}
		}

		if (at_pipe)
			sh_wordv_push(&expanded, sh_xstrdup("|"));

		stage_start = i + 1;
	}

	sh_wordv_free(&cmd->words);
	cmd->words = expanded;
	return 0;
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
