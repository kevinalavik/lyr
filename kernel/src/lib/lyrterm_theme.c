#include <lib/lyrterm_theme.h>

const lyrterm_theme_t lyrterm_theme_dark = {
    .name = "dark",
    .fg   = 0x00f5e6c8,
    .bg   = 0x001e1e1e,
    .ansi_normal = {
        0x00000000, 0x00cc3333, 0x0066aa44, 0x00cc8833,
        0x004477cc, 0x00aa44aa, 0x0044aaaa, 0x00aaaaaa,
    },
    .ansi_bright = {
        0x00555555, 0x00ff6666, 0x0088dd55, 0x00ffdd55,
        0x005588ff, 0x00ff66ff, 0x0055ffee, 0x00ffffff,
    },
};

const lyrterm_theme_t lyrterm_theme_light = {
    .name = "light",
    .fg   = 0x001e1e1e,
    .bg   = 0x00f5e6c8,
    .ansi_normal = {
        0x00222222, 0x00aa1111, 0x00227722, 0x00886600,
        0x001144aa, 0x00882288, 0x00117777, 0x00555555,
    },
    .ansi_bright = {
        0x00444444, 0x00cc2222, 0x0033aa33, 0x00aa8800,
        0x002255cc, 0x00aa33aa, 0x0022aaaa, 0x00111111,
    },
};