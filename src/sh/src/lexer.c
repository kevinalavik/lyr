#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sh.h>

#define SH_NOEXPAND_BEGIN '\001'
#define SH_NOEXPAND_END '\002'

static void sb_add(char **buf, size_t *len, size_t *cap, char c)
{
	if (*len + 1 >= *cap) {
		*cap = *cap ? *cap * 2 : 64;
		*buf = sh_xrealloc(*buf, *cap);
	}

	(*buf)[(*len)++] = c;
	(*buf)[*len] = '\0';
}

static void sb_start_word(char **buf, size_t *len, size_t *cap)
{
	if (!*buf) {
		sb_add(buf, len, cap, '\0');
		*len = 0;
	}
}

static int finish_word(sh_wordv_t *words, char **buf, size_t *len, size_t *cap)
{
	(void)cap;

	if (!*buf)
		return 0;

	if (*len == 0) {
		free(*buf);
		*buf = NULL;
		*cap = 0;
		return 0;
	}

	char *expanded = sh_expand_vars(*buf);

	free(*buf);
	*buf = NULL;
	*len = 0;
	*cap = 0;

	sh_wordv_push(words, expanded);
	return 0;
}

static void finish_command(sh_command_list_t *out, sh_command_t *cmd)
{
	if (cmd->words.n == 0)
		return;

	sh_command_list_push(out, *cmd);
	sh_wordv_init(&cmd->words);
}

int sh_parse_line(const char *line, sh_command_list_t *out, char **err)
{
	sh_command_list_init(out);

	if (err)
		*err = NULL;

	sh_command_t cmd;
	sh_wordv_init(&cmd.words);

	char *word = NULL;
	size_t wlen = 0;
	size_t wcap = 0;

	int sq = 0;
	int dq = 0;
	int esc = 0;

	for (size_t i = 0;; i++) {
		char c = line[i];

		if (esc) {
			sb_start_word(&word, &wlen, &wcap);
			sb_add(&word, &wlen, &wcap, c);
			esc = 0;

			if (!c)
				break;

			continue;
		}

		if (c == '\\' && !sq) {
			if (dq) {
				char next = line[i + 1];

				if (next == '$' || next == '`' || next == '"' || next == '\\' ||
					next == '\n') {
					esc = 1;
					continue;
				}

				sb_start_word(&word, &wlen, &wcap);
				sb_add(&word, &wlen, &wcap, c);
				continue;
			}

			esc = 1;
			continue;
		}

		if (c == '\'' && !dq) {
			sb_start_word(&word, &wlen, &wcap);

			if (!sq) {
				sb_add(&word, &wlen, &wcap, SH_NOEXPAND_BEGIN);
				sq = 1;
			} else {
				sb_add(&word, &wlen, &wcap, SH_NOEXPAND_END);
				sq = 0;
			}

			continue;
		}

		if (c == '"' && !sq) {
			sb_start_word(&word, &wlen, &wcap);
			dq = !dq;
			continue;
		}

		if (!sq && !dq && c == '#') {
			while (line[i] && line[i] != '\n')
				i++;

			c = line[i];
		}

		if (!c || (!sq && !dq && (c == '\n' || c == ';'))) {
			finish_word(&cmd.words, &word, &wlen, &wcap);
			finish_command(out, &cmd);

			if (!c)
				break;

			continue;
		}

		if (!sq && !dq && isspace((unsigned char)c)) {
			finish_word(&cmd.words, &word, &wlen, &wcap);
			continue;
		}

		if (!sq && !dq && c == '|') {
			finish_word(&cmd.words, &word, &wlen, &wcap);
			sh_wordv_push(&cmd.words, sh_xstrdup("|"));
			continue;
		}

		sb_start_word(&word, &wlen, &wcap);
		sb_add(&word, &wlen, &wcap, c);
	}

	if (sq || dq || esc) {
		if (err)
			*err = sh_xstrdup("unterminated quote or escape");

		free(word);
		sh_wordv_free(&cmd.words);
		sh_command_list_free(out);
		return -1;
	}

	free(word);
	return 0;
}
