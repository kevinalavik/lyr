#include <lib/lyrterm.h>
#include <lib/lyrterm_font.h>
#include <lib/string.h>
#include <util/kprintf.h>

#include <stdbool.h>
#include <stddef.h>

static volatile uint32_t *fb;
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t fb_pitch;

static uint32_t cursor_x;
static uint32_t cursor_y;

static bool initialized = false;

static uint32_t cols;
static uint32_t rows;

static uint32_t default_fg = 0x00f5e6c8;
static uint32_t default_bg = 0x001e1e1e;

static uint32_t current_fg;
static uint32_t current_bg;

#define CURSOR_COLOR 0x00000000
#define CURSOR_HEIGHT 2

#define ANSI_MAX_PARAMS 8

typedef enum {
	ANSI_STATE_NORMAL,
	ANSI_STATE_ESC,
	ANSI_STATE_CSI,
} ansi_state_t;

typedef void (*ansi_handler_t)(int *params, int nparams);

typedef struct {
	char final;
	ansi_handler_t handler;
} ansi_csi_handler_t;

static ansi_state_t ansi_state = ANSI_STATE_NORMAL;
static int ansi_params[ANSI_MAX_PARAMS];
static int ansi_nparams;

static const uint32_t ansi_colors[8] = {
	0x00000000, 0x00aa0000, 0x0000aa00, 0x00aa5500,
	0x000000aa, 0x00aa00aa, 0x0000aaaa, 0x00aaaaaa,
};

static const uint32_t ansi_colors_bright[8] = {
	0x00555555, 0x00ff5555, 0x0055ff55, 0x00ffff55,
	0x005555ff, 0x00ff55ff, 0x0055ffff, 0x00ffffff,
};

static void putpixel(uint32_t x, uint32_t y, uint32_t color)
{
	if (x < fb_width && y < fb_height)
		fb[y * fb_pitch + x] = color;
}

static void drawch(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg)
{
	if ((unsigned char)c < 32 || (unsigned char)c > 126)
		c = '?';

	const uint8_t glyph_row_base = (uint8_t)c - 32;

	for (uint32_t row = 0; row < _LYRTERM_FONT_HEIGHT; row++) {
		uint8_t bits = _lyrterm_font[glyph_row_base][row];
		for (uint32_t col = 0; col < _LYRTERM_FONT_WIDTH; col++) {
			uint32_t color = (bits & (1 << (7 - col))) ? fg : bg;
			putpixel(x + col, y + row, color);
		}
	}
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
					  uint32_t color)
{
	for (uint32_t row = y; row < y + h; row++)
		for (uint32_t col = x; col < x + w; col++)
			putpixel(col, row, color);
}

static void scroll_up(void)
{
	const uint32_t copy_rows = fb_height - _LYRTERM_FONT_HEIGHT;

	for (uint32_t y = 0; y < copy_rows; y++)
		memcpy((void *)&fb[y * fb_pitch],
			   (void *)&fb[(y + _LYRTERM_FONT_HEIGHT) * fb_pitch],
			   fb_width * sizeof(uint32_t));

	fill_rect(0, copy_rows, fb_width, _LYRTERM_FONT_HEIGHT, default_bg);
}

static void cursor_draw(void)
{
	fill_rect(cursor_x, cursor_y + _LYRTERM_FONT_HEIGHT - CURSOR_HEIGHT,
			  _LYRTERM_FONT_WIDTH, CURSOR_HEIGHT, CURSOR_COLOR);
}

static void cursor_erase(void)
{
	fill_rect(cursor_x, cursor_y + _LYRTERM_FONT_HEIGHT - CURSOR_HEIGHT,
			  _LYRTERM_FONT_WIDTH, CURSOR_HEIGHT, current_bg);
}

static void newline(void)
{
	cursor_x = 0;
	cursor_y += _LYRTERM_FONT_HEIGHT;

	if (cursor_y + _LYRTERM_FONT_HEIGHT > fb_height) {
		scroll_up();
		cursor_y = (rows - 1) * _LYRTERM_FONT_HEIGHT;
	}
}

static void advance_cursor(void)
{
	cursor_x += _LYRTERM_FONT_WIDTH;

	if (cursor_x + _LYRTERM_FONT_WIDTH > fb_width)
		newline();
}

static void ansi_handle_sgr(int *params, int nparams)
{
	if (nparams == 0) {
		current_fg = default_fg;
		current_bg = default_bg;
		return;
	}

	for (int i = 0; i < nparams; i++) {
		int p = params[i];

		if (p == 0) {
			current_fg = default_fg;
			current_bg = default_bg;
		} else if (p >= 30 && p <= 37) {
			current_fg = ansi_colors[p - 30];
		} else if (p >= 40 && p <= 47) {
			current_bg = ansi_colors[p - 40];
		} else if (p >= 90 && p <= 97) {
			current_fg = ansi_colors_bright[p - 90];
		} else if (p >= 100 && p <= 107) {
			current_bg = ansi_colors_bright[p - 100];
		}
	}
}

static const ansi_csi_handler_t ansi_csi_handlers[] = {
	{ 'm', ansi_handle_sgr },
};

static void ansi_dispatch_csi(char final)
{
	int nparams =
		(ansi_params[ansi_nparams] || ansi_nparams) ? ansi_nparams + 1 : 0;

	for (size_t i = 0;
		 i < sizeof(ansi_csi_handlers) / sizeof(*ansi_csi_handlers); i++) {
		if (ansi_csi_handlers[i].final == final) {
			ansi_csi_handlers[i].handler(ansi_params, nparams);
			return;
		}
	}
}

void lyrterm_init(volatile uint32_t *framebuffer, uint32_t width,
				  uint32_t height, uint32_t pitch)
{
	if (!framebuffer || !width || !height || !pitch) {
		kprintf("lyrterm: invalid framebuffer parameters\n");
		return;
	}

	fb = framebuffer;
	fb_width = width;
	fb_height = height;
	fb_pitch = pitch;
	cursor_x = 0;
	cursor_y = 0;
	cols = width / _LYRTERM_FONT_WIDTH;
	rows = height / _LYRTERM_FONT_HEIGHT;
	current_fg = default_fg;
	current_bg = default_bg;

	for (uint32_t y = 0; y < height; y++)
		for (uint32_t x = 0; x < width; x++)
			fb[y * pitch + x] = default_bg;

	initialized = true;
	cursor_draw();
}

void lyrterm_set_colors(uint32_t fg, uint32_t bg)
{
	default_fg = fg;
	default_bg = bg;
	current_fg = fg;
	current_bg = bg;
}

void lyrterm_putch(char c)
{
	if (!initialized)
		return;

	if (ansi_state == ANSI_STATE_ESC) {
		ansi_state = (c == '[') ? ANSI_STATE_CSI : ANSI_STATE_NORMAL;
		if (ansi_state == ANSI_STATE_CSI) {
			ansi_nparams = 0;
			ansi_params[0] = 0;
		}
		return;
	}

	if (ansi_state == ANSI_STATE_CSI) {
		if (c >= '0' && c <= '9') {
			ansi_params[ansi_nparams] =
				ansi_params[ansi_nparams] * 10 + (c - '0');
		} else if (c == ';') {
			if (ansi_nparams < ANSI_MAX_PARAMS - 1)
				ansi_params[++ansi_nparams] = 0;
		} else {
			ansi_dispatch_csi(c);
			ansi_nparams = 0;
			ansi_state = ANSI_STATE_NORMAL;
		}
		return;
	}

	if (c == '\e') {
		ansi_state = ANSI_STATE_ESC;
		return;
	}

	cursor_erase();

	switch (c) {
	case '\n':
		newline();
		break;
	case '\r':
		cursor_x = 0;
		break;
	case '\t':
		do {
			advance_cursor();
		} while ((cursor_x / _LYRTERM_FONT_WIDTH) % 8);
		break;
	default:
		drawch(cursor_x, cursor_y, c, current_fg, current_bg);
		advance_cursor();
		break;
	}

	cursor_draw();
}

void lyrterm_putstr(const char *str)
{
	if (!str)
		return;

	while (*str)
		lyrterm_putch(*str++);
}