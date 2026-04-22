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

const lyrterm_theme_t lyrterm_theme_solarized_dark = {
    .name = "solarized_dark",
    .fg   = 0x008396a7,
    .bg   = 0x00002b36,
    .ansi_normal = {
        0x00073642, 0x00dc322f, 0x00859900, 0x00b58900,
        0x002682bd, 0x00d33682, 0x002aa198, 0x00eee8d5,
    },
    .ansi_bright = {
        0x00002b36, 0x00cb4b16, 0x00586e75, 0x00657b83,
        0x008396a7, 0x006c71c4, 0x0093a1a1, 0x00fdf6e3,
    },
};

const lyrterm_theme_t lyrterm_theme_solarized_light = {
    .name = "solarized_light",
    .fg   = 0x00657b83,
    .bg   = 0x00fdf6e3,
    .ansi_normal = {
        0x00073642, 0x00dc322f, 0x00859900, 0x00b58900,
        0x002682bd, 0x00d33682, 0x002aa198, 0x00eee8d5,
    },
    .ansi_bright = {
        0x00002b36, 0x00cb4b16, 0x00586e75, 0x00657b83,
        0x008396a7, 0x006c71c4, 0x0093a1a1, 0x00fdf6e3,
    },
};

const lyrterm_theme_t lyrterm_theme_gruvbox_dark = {
    .name = "gruvbox_dark",
    .fg   = 0x00ebdbb2,
    .bg   = 0x00282828,
    .ansi_normal = {
        0x00282828, 0x00cc241d, 0x00989a24, 0x00d79921,
        0x00458888, 0x00b16286, 0x00689d6a, 0x00a89984,
    },
    .ansi_bright = {
        0x00928974, 0x00fb4934, 0x00b8bb26, 0x00fabd2f,
        0x0083a598, 0x00d3869b, 0x008ec07c, 0x00fbf1c7,
    },
};

const lyrterm_theme_t lyrterm_theme_gruvbox_light = {
    .name = "gruvbox_light",
    .fg   = 0x00282828,
    .bg   = 0x00ebdbb2,
    .ansi_normal = {
        0x00282828, 0x00cc241d, 0x00989a24, 0x00d79921,
        0x00458888, 0x00b16286, 0x00689d6a, 0x00a89984,
    },
    .ansi_bright = {
        0x00928974, 0x00fb4934, 0x00b8bb26, 0x00fabd2f,
        0x0083a598, 0x00d3869b, 0x008ec07c, 0x00fbf1c7,
    },
};

const lyrterm_theme_t lyrterm_theme_dracula = {
    .name = "dracula",
    .fg   = 0x00f8f8f2,
    .bg   = 0x00282736,
    .ansi_normal = {
        0x00262736, 0x00ff5555, 0x0050fa7b, 0x00f1fa8c,
        0x00bd93f9, 0x00ff79c6, 0x008be9fd, 0x00bbbbbb,
    },
    .ansi_bright = {
        0x00444475, 0x00ff6e6e, 0x0069ff94, 0x00ffffa5,
        0x00d6acff, 0x00ff92df, 0x00a4ffff, 0x00ffffff,
    },
};

const lyrterm_theme_t lyrterm_theme_neon = {
    .name = "neon",
    .fg   = 0x00e0e0e0,
    .bg   = 0x00000000,
    .ansi_normal = {
        0x00000000, 0x00ff0055, 0x0000ff9c, 0x00ffee00,
        0x000066ff, 0x00ff00ff, 0x0000ffff, 0x00cccccc,
    },
    .ansi_bright = {
        0x00444444, 0x00ff3377, 0x0033ffbb, 0x00ffff66,
        0x003399ff, 0x00ff66ff, 0x0066ffff, 0x00ffffff,
    },
};

const lyrterm_theme_t lyrterm_theme_nord = {
    .name = "nord",
    .fg   = 0x00d8dee9,
    .bg   = 0x002e3440,
    .ansi_normal = {
        0x003b4252, 0x00bf616a, 0x00a3be8c, 0x00ebcb8b,
        0x0081a1c1, 0x00b48ead, 0x0088c0d0, 0x00e5e9f0,
    },
    .ansi_bright = {
        0x004c566a, 0x00bf616a, 0x00a3be8c, 0x00ebcb8b,
        0x0081a1c1, 0x00b48ead, 0x0088c0d0, 0x00eceff4,
    },
};