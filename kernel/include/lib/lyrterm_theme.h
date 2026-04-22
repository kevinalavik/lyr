#ifndef _LYR_LIB_LYRTERM_THEME_H
#define _LYR_LIB_LYRTERM_THEME_H

#include <stdint.h>

typedef struct {
    const char *name;
    uint32_t fg;
    uint32_t bg;
    uint32_t ansi_normal[8];
    uint32_t ansi_bright[8];
} lyrterm_theme_t;

extern const lyrterm_theme_t lyrterm_theme_dark;
extern const lyrterm_theme_t lyrterm_theme_light;

void lyrterm_apply_theme(const lyrterm_theme_t *theme);

#endif // _LYR_LIB_LYRTERM_THEME_H