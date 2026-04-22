#ifndef _LYR_LIB_LYRTERM_H
#define _LYR_LIB_LYRTERM_H

#include <stdint.h>
#include <lib/lyrterm_theme.h>

/* lyr kernel graphical terminal renderer */

void lyrterm_init(volatile uint32_t *framebuffer, uint32_t width,
				  uint32_t height, uint32_t pitch);
void lyrterm_putch(char c);
void lyrterm_putstr(const char *str);
void lyrterm_apply_theme(const lyrterm_theme_t *theme);

#endif // _LYR_LIB_LYRTERM_H