#ifndef _LYR_LIB_LYRTERM_H
#define _LYR_LIB_LYRTERM_H

#include <stddef.h>
#include <stdint.h>
#include <lib/lyrterm_theme.h>
#include <limine.h>

/* for line cursor, uncomment this line */
// #define LYRTERM_LINE_CURSOR

/* lyr kernel graphical terminal renderer */
void lyrterm_init(const struct limine_framebuffer *fb);
void lyrterm_putch(char c);
void lyrterm_putstr(const char *str);
void lyrterm_wbuf(const char *buf, size_t len);
size_t lyrterm_drain(size_t budget);
void lyrterm_flush(void);
size_t lyrterm_dropped_bytes(void);
void lyrterm_apply_theme(const lyrterm_theme_t *theme);

#endif // _LYR_LIB_LYRTERM_H
