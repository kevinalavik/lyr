#include <stdio.h>
#include <string.h>

#include "script.h"

static char lx_peek(struct lexer *lx)
{
	return lx->src[lx->pos];
}

static char lx_get(struct lexer *lx)
{
	char c = lx->src[lx->pos];

	if (c == '\0')
		return '\0';

	lx->pos++;

	if (c == '\n') {
		lx->line++;
		lx->column = 1;
	} else {
		lx->column++;
	}

	return c;
}

static void token_set_error(struct token *tok, struct lexer *lx,
			    const char *msg)
{
	tok->type = TOK_ERROR;
	tok->line = lx->line;
	tok->column = lx->column;

	snprintf(tok->text, sizeof(tok->text), "%s", msg);
	snprintf(lx->error, sizeof(lx->error), "%s", msg);
}

void lexer_init(struct lexer *lx, const char *path, const char *src)
{
	lx->path = path;
	lx->src = src;
	lx->pos = 0;
	lx->line = 1;
	lx->column = 1;
	lx->error[0] = '\0';
}

struct token lexer_next(struct lexer *lx)
{
	struct token tok;
	size_t out = 0;

	memset(&tok, 0, sizeof(tok));

	for (;;) {
		char c = lx_peek(lx);

		if (c == ' ' || c == '\t' || c == '\r') {
			lx_get(lx);
			continue;
		}

		if (c == '#') {
			while (lx_peek(lx) && lx_peek(lx) != '\n')
				lx_get(lx);
			continue;
		}

		break;
	}

	tok.line = lx->line;
	tok.column = lx->column;

	if (lx_peek(lx) == '\0') {
		tok.type = TOK_EOF;
		return tok;
	}

	if (lx_peek(lx) == '\n') {
		lx_get(lx);
		tok.type = TOK_NEWLINE;
		return tok;
	}

	if (lx_peek(lx) == ';') {
		lx_get(lx);
		tok.type = TOK_SEMICOLON;
		return tok;
	}

	tok.type = TOK_WORD;

	while (lx_peek(lx)) {
		char c = lx_peek(lx);

		if (c == ' ' || c == '\t' || c == '\r' ||
		    c == '\n' || c == ';' || c == '#')
			break;

		if (out + 1 >= sizeof(tok.text)) {
			token_set_error(&tok, lx, "token too long");
			return tok;
		}

		if (c == '\'') {
			lx_get(lx);

			while (lx_peek(lx) && lx_peek(lx) != '\'') {
				if (out + 1 >= sizeof(tok.text)) {
					token_set_error(&tok, lx, "token too long");
					return tok;
				}

				tok.text[out++] = lx_get(lx);
			}

			if (lx_peek(lx) != '\'') {
				token_set_error(&tok, lx, "unterminated single quote");
				return tok;
			}

			lx_get(lx);
			continue;
		}

		if (c == '"') {
			lx_get(lx);

			while (lx_peek(lx) && lx_peek(lx) != '"') {
				c = lx_get(lx);

				if (c == '\\') {
					char next = lx_get(lx);

					if (next == '\0') {
						token_set_error(&tok, lx,
								"unterminated escape");
						return tok;
					}

					switch (next) {
					case 'n':
						c = '\n';
						break;
					case 't':
						c = '\t';
						break;
					case '"':
					case '\\':
					case '$':
						c = next;
						break;
					default:
						c = next;
						break;
					}
				}

				if (out + 1 >= sizeof(tok.text)) {
					token_set_error(&tok, lx, "token too long");
					return tok;
				}

				tok.text[out++] = c;
			}

			if (lx_peek(lx) != '"') {
				token_set_error(&tok, lx, "unterminated double quote");
				return tok;
			}

			lx_get(lx);
			continue;
		}

		if (c == '\\') {
			lx_get(lx);
			c = lx_get(lx);

			if (c == '\0') {
				token_set_error(&tok, lx, "unterminated escape");
				return tok;
			}

			tok.text[out++] = c;
			continue;
		}

		tok.text[out++] = lx_get(lx);
	}

	tok.text[out] = '\0';
	return tok;
}
