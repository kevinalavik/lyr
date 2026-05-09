#ifndef TERM_H
#define TERM_H

#include <stddef.h>

void term_clear(void);
int term_read_line(const char *prompt, char *buf, size_t size);
int term_read_password(const char *prompt, char *buf, size_t size);
void term_clear_string(char *s);
void term_trim_spaces(char *s);
void term_trim_newline(char *s);
void term_println(const char *ansi_code, const char *msg);

#endif /* TERM_H */
