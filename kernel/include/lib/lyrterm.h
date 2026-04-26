#ifndef _LYR_LIB_LYRTERM_H
#define _LYR_LIB_LYRTERM_H

#include <stdint.h>
#include <lib/lyrterm_theme.h>
#include <limine.h>

/* for line cursor, uncomment this line */
// #define LYRTERM_LINE_CURSOR

/* lyr kernel graphical terminal renderer */
void lyrterm_init(const struct limine_framebuffer *fb);
void lyrterm_putch(char c);
void lyrterm_putstr(const char *str);
void lyrterm_apply_theme(const lyrterm_theme_t *theme);

#endif // _LYR_LIB_LYRTERM_H