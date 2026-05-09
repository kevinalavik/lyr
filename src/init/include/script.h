#ifndef SCRIPT_H
#define SCRIPT_H

#include <stddef.h>

#define MAX_ARGS        32
#define MAX_ENV         64
#define MAX_INCLUDE     8
#define MAX_TOKEN       256

enum token_type {
	TOK_EOF = 0,
	TOK_WORD,
	TOK_NEWLINE,
	TOK_SEMICOLON,
	TOK_ERROR,
};

struct token {
	enum token_type type;
	char text[MAX_TOKEN];
	int line;
	int column;
};

struct lexer {
	const char *path;
	const char *src;
	size_t pos;
	int line;
	int column;
	char error[256];
};

struct command {
	int argc;
	char *argv[MAX_ARGS];
	int line;
};

struct script {
	struct command *commands;
	size_t count;
	size_t capacity;
};

struct env {
	char *vars[MAX_ENV];
	size_t count;
};

struct runtime {
	struct env env;
	int include_depth;
};

/* lexer */
void lexer_init(struct lexer *lx, const char *path, const char *src);
struct token lexer_next(struct lexer *lx);

/* parser */
int parse_file(const char *path, struct script *script);
void script_init(struct script *script);
void script_destroy(struct script *script);

/* executor */
int execute_script(struct runtime *rt, const struct script *script);
void init_reap_forever(void);
void runtime_init(struct runtime *rt);
void runtime_destroy(struct runtime *rt);

#endif
