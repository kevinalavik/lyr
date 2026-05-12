#include <ctype.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sh.h>

static void append_char(char **out, size_t *len, size_t *cap, char c)
{
	if (*len + 1 >= *cap) {
		*cap = *cap ? *cap * 2 : 64;
		*out = sh_xrealloc(*out, *cap);
	}

	(*out)[(*len)++] = c;
	(*out)[*len] = '\0';
}

static void append_str(char **out, size_t *len, size_t *cap, const char *s)
{
	while (s && *s)
		append_char(out, len, cap, *s++);
}

static int octal_value(char c)
{
	if (c < '0' || c > '7')
		return -1;

	return c - '0';
}

static int parse_octal_escape(const char *s, size_t *used)
{
	int value = 0;
	size_t i = 0;

	while (i < 3) {
		int v = octal_value(s[i]);

		if (v < 0)
			break;

		value = value * 8 + v;
		i++;
	}

	if (i == 0)
		return -1;

	*used = i;
	return value & 0xff;
}

char *sh_expand_vars(const char *input)
{
	char *out = NULL;
	size_t len = 0;
	size_t cap = 0;
	int noexpand = 0;

	append_char(&out, &len, &cap, '\0');
	len = 0;

	for (size_t i = 0; input[i]; i++) {
		if (input[i] == SH_NOEXPAND_BEGIN) {
			noexpand = 1;
			continue;
		}

		if (input[i] == SH_NOEXPAND_END) {
			noexpand = 0;
			continue;
		}

		if (noexpand || input[i] != '$') {
			append_char(&out, &len, &cap, input[i]);
			continue;
		}

		i++;

		if (input[i] == '$') {
			char tmp[32];
			snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)getpid());
			append_str(&out, &len, &cap, tmp);
		} else if (input[i] == '?') {
			const char *v = getenv("?");
			append_str(&out, &len, &cap, v ? v : "0");
		} else if (input[i] == '{') {
			size_t start = ++i;

			while (input[i] && input[i] != '}')
				i++;

			char *name = sh_strndup(input + start, i - start);
			const char *v = getenv(name);

			append_str(&out, &len, &cap, v ? v : "");
			free(name);

			if (!input[i])
				break;
		} else if (isalpha((unsigned char)input[i]) || input[i] == '_') {
			size_t start = i;

			while (isalnum((unsigned char)input[i]) || input[i] == '_')
				i++;

			char *name = sh_strndup(input + start, i - start);
			const char *v = getenv(name);

			append_str(&out, &len, &cap, v ? v : "");
			free(name);
			i--;
		} else {
			append_char(&out, &len, &cap, '$');

			if (input[i])
				append_char(&out, &len, &cap, input[i]);
			else
				break;
		}
	}

	return out;
}

static void append_prompt_cwd(char **out, size_t *len, size_t *cap,
							  const char *cwd)
{
	const char *home = getenv("HOME");

	if (home && home[0]) {
		size_t home_len = strlen(home);

		if (strcmp(cwd, home) == 0) {
			append_char(out, len, cap, '~');
		} else if (strncmp(cwd, home, home_len) == 0 && cwd[home_len] == '/') {
			append_char(out, len, cap, '~');
			append_str(out, len, cap, cwd + home_len);
		} else {
			append_str(out, len, cap, cwd);
		}
	} else {
		append_str(out, len, cap, cwd);
	}
}

static void append_prompt_basename(char **out, size_t *len, size_t *cap,
								   const char *cwd)
{
	const char *home = getenv("HOME");

	if (home && strcmp(cwd, home) == 0) {
		append_char(out, len, cap, '~');
		return;
	}

	const char *base = strrchr(cwd, '/');
	append_str(out, len, cap, base && base[1] ? base + 1 : cwd);
}

char *sh_expand_prompt(const char *ps1)
{
	if (!ps1)
		ps1 = "\\u@\\h:\\w\\$ ";

	char *cwd = sh_getcwd_alloc();

	const char *user = getenv("USER");
	if (!user)
		user = "user";

	const char *host = getenv("HOSTNAME");
	if (!host)
		host = "lyr";

	char *out = NULL;
	size_t len = 0;
	size_t cap = 0;

	append_char(&out, &len, &cap, '\0');
	len = 0;

	for (size_t i = 0; ps1[i]; i++) {
		if (ps1[i] == '$') {
			char *rest = sh_expand_vars(ps1 + i);
			append_str(&out, &len, &cap, rest);
			free(rest);
			break;
		}

		if (ps1[i] != '\\') {
			append_char(&out, &len, &cap, ps1[i]);
			continue;
		}

		i++;

		switch (ps1[i]) {
		case '[':
		case ']':
			break;

		case 'u':
			append_str(&out, &len, &cap, user);
			break;

		case 'h':
			append_str(&out, &len, &cap, host);
			break;

		case 'H':
			append_str(&out, &len, &cap, host);
			break;

		case 'w':
			append_prompt_cwd(&out, &len, &cap, cwd);
			break;

		case 'W':
			append_prompt_basename(&out, &len, &cap, cwd);
			break;

		case '$':
			append_char(&out, &len, &cap, getuid() == 0 ? '#' : '$');
			break;

		case 'n':
			append_char(&out, &len, &cap, '\n');
			break;

		case 'r':
			append_char(&out, &len, &cap, '\r');
			break;

		case 't':
			append_char(&out, &len, &cap, '\t');
			break;

		case 'a':
			append_char(&out, &len, &cap, '\a');
			break;

		case 'e':
		case 'E':
			append_char(&out, &len, &cap, '\033');
			break;

		case '0' ... '7': {
			size_t used = 0;
			int value = parse_octal_escape(ps1 + i, &used);

			if (value >= 0) {
				append_char(&out, &len, &cap, (char)value);
				i += used - 1;
			} else {
				append_char(&out, &len, &cap, ps1[i]);
			}

			break;
		}

		case '\\':
			append_char(&out, &len, &cap, '\\');
			break;

		case '\0':
			i--;
			break;

		default:
			append_char(&out, &len, &cap, '\\');
			append_char(&out, &len, &cap, ps1[i]);
			break;
		}
	}

	free(cwd);
	return out;
}
