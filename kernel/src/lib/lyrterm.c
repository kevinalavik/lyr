#include <lib/lyrterm.h>
#include <lib/lyrterm_font.h>
#include <lib/string.h>
#include <util/kprintf.h>
#include <lib/lyrterm_theme.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define _LYRTERM_LINE_PADDING_Y 0
#define _LYRTERM_LINE_HEIGHT \
	(_LYRTERM_FONT_ASCENT + _LYRTERM_FONT_DESCENT + _LYRTERM_LINE_PADDING_Y)

#define _LYRTERM_LINE_PADDING_X 0
#define _LYRTERM_LINE_WIDTH (_LYRTERM_FONT_WIDTH + _LYRTERM_LINE_PADDING_X)

#define _LYRTERM_MARGIN_X 10
#define _LYRTERM_MARGIN_Y 10

static uint32_t *fb;
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t fb_pitch;

static uint32_t cursor_x;
static uint32_t cursor_y;

static bool initialized = false;

static uint32_t cols;
static uint32_t rows;

static const lyrterm_theme_t *active_theme = &lyrterm_theme_dark;

static uint32_t default_fg;
static uint32_t default_bg;
static uint32_t current_fg;
static uint32_t current_bg;

#define ansi_colors (active_theme->ansi_normal)
#define ansi_colors_bright (active_theme->ansi_bright)

#define CURSOR_COLOR current_fg
#ifdef LYRTERM_LINE_CURSOR
#define CURSOR_HEIGHT _LYRTERM_FONT_HEIGHT / 6
#else
#define CURSOR_HEIGHT _LYRTERM_FONT_HEIGHT
#endif

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

typedef struct {
	uint32_t codepoint;
	uint8_t bytes_left;
} utf8_state_t;

static utf8_state_t utf8 = { 0, 0 };

static uint32_t utf8_feed(uint8_t byte)
{
	if (utf8.bytes_left == 0) {
		if (byte < 0x80) {
			return (uint32_t)byte;
		} else if ((byte & 0xE0) == 0xC0) {
			utf8.codepoint = byte & 0x1F;
			utf8.bytes_left = 1;
		} else if ((byte & 0xF0) == 0xE0) {
			utf8.codepoint = byte & 0x0F;
			utf8.bytes_left = 2;
		} else if ((byte & 0xF8) == 0xF0) {
			utf8.codepoint = byte & 0x07;
			utf8.bytes_left = 3;
		} else {
			return 0xFFFD;
		}
		return 0;
	}

	if ((byte & 0xC0) != 0x80) {
		utf8.bytes_left = 0;
		utf8.codepoint = 0;
		return 0xFFFD;
	}

	utf8.codepoint = (utf8.codepoint << 6) | (byte & 0x3F);
	utf8.bytes_left--;

	if (utf8.bytes_left == 0) {
		uint32_t cp = utf8.codepoint;
		utf8.codepoint = 0;
		if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
			return 0xFFFD;
		return cp;
	}
	return 0;
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
					  uint32_t color)
{
	for (uint32_t row = 0; row < h; row++) {
		uint32_t *p = &fb[(y + row) * fb_pitch + x];
		uint32_t n = w;
		__asm__ volatile("rep stosl"
						 : "+D"(p), "+c"(n)
						 : "a"(color)
						 : "memory");
	}
}

static void drawch(uint32_t x, uint32_t y, uint32_t codepoint, uint32_t fg,
				   uint32_t bg)
{
	int glyph_index = _lyrterm_find_glyph(codepoint);
	const uint8_t *glyph = (glyph_index >= 0) ? _lyrterm_font[glyph_index] :
												_lyrterm_font_sentinel;

	const uint32_t bytes_per_row = (_LYRTERM_FONT_WIDTH + 7) / 8;

	for (uint32_t row = 0; row < _LYRTERM_FONT_HEIGHT; row++) {
		const uint8_t *row_data = &glyph[row * bytes_per_row];
		uint32_t *dst = &fb[(y + row) * fb_pitch + x];
		uint32_t col = 0;

		for (uint32_t b = 0; b < bytes_per_row; b++) {
			uint8_t byte = row_data[b];
			uint32_t remaining = _LYRTERM_FONT_WIDTH - col;
			uint32_t bits = remaining < 8 ? remaining : 8;
			for (uint32_t bit = 0; bit < bits; bit++) {
				dst[col++] = (byte & 0x80) ? fg : bg;
				byte <<= 1;
			}
		}
	}
}
static inline uint32_t term_x0(void)
{
	return _LYRTERM_MARGIN_X;
}
static inline uint32_t term_y0(void)
{
	return _LYRTERM_MARGIN_Y;
}
static inline uint32_t term_width(void)
{
	return fb_width - (_LYRTERM_MARGIN_X * 2);
}
static inline uint32_t term_height(void)
{
	return fb_height - (_LYRTERM_MARGIN_Y * 2);
}

static void scroll_up(void)
{
	uint32_t x0 = term_x0();
	uint32_t y0 = term_y0();
	uint32_t w = term_width();
	uint32_t h = term_height();

	if (x0 == 0 && w == fb_width) {
		uint32_t *dst = &fb[y0 * fb_pitch];
		uint32_t *src = &fb[(y0 + _LYRTERM_LINE_HEIGHT) * fb_pitch];
		size_t count = (h - _LYRTERM_LINE_HEIGHT) * fb_pitch;
		memcpy(dst, src, count * sizeof(uint32_t));
	} else {
		uint32_t rows_to_copy = h - _LYRTERM_LINE_HEIGHT;
		for (uint32_t row = 0; row < rows_to_copy; row++) {
			uint32_t *dst = &fb[(y0 + row) * fb_pitch + x0];
			uint32_t *src =
				&fb[(y0 + row + _LYRTERM_LINE_HEIGHT) * fb_pitch + x0];
			memcpy(dst, src, w * sizeof(uint32_t));
		}
	}

	fill_rect(x0, y0 + (h - _LYRTERM_LINE_HEIGHT), w, _LYRTERM_LINE_HEIGHT,
			  default_bg);
}

static void cursor_draw(void)
{
	uint32_t top = cursor_y - _LYRTERM_FONT_ASCENT;

	fill_rect(cursor_x, top + (_LYRTERM_FONT_HEIGHT - CURSOR_HEIGHT),
			  _LYRTERM_FONT_WIDTH, CURSOR_HEIGHT, CURSOR_COLOR);
}

static void cursor_erase(void)
{
	uint32_t top = cursor_y - _LYRTERM_FONT_ASCENT;

	fill_rect(cursor_x, top + (_LYRTERM_FONT_HEIGHT - CURSOR_HEIGHT),
			  _LYRTERM_FONT_WIDTH, CURSOR_HEIGHT, current_bg);
}

static void newline(void)
{
	cursor_x = term_x0();
	cursor_y += _LYRTERM_LINE_HEIGHT;

	if (cursor_y + _LYRTERM_FONT_DESCENT > term_y0() + term_height()) {
		scroll_up();
		cursor_y = term_y0() + (rows - 1) * _LYRTERM_LINE_HEIGHT +
				   _LYRTERM_FONT_ASCENT;
	}
}

static void advance_cursor(void)
{
	cursor_x += _LYRTERM_FONT_WIDTH;
	if (cursor_x + _LYRTERM_FONT_WIDTH > term_x0() + term_width())
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
	if (!framebuffer || !width || !height || !pitch)
		return;

	fb = (uint32_t *)framebuffer;
	fb_width = width;
	fb_height = height;
	fb_pitch = pitch;

	cursor_x = term_x0();
	cursor_y = term_y0() + _LYRTERM_FONT_ASCENT;

	cols = term_width() / _LYRTERM_LINE_WIDTH;
	rows = term_height() / _LYRTERM_LINE_HEIGHT;

	lyrterm_apply_theme(active_theme);

	fill_rect(0, 0, fb_width, fb_height, default_bg);

	utf8.codepoint = 0;
	utf8.bytes_left = 0;

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

void lyrterm_putch(char raw)
{
	if (!initialized)
		return;

	uint8_t byte = (uint8_t)raw;

	if (ansi_state == ANSI_STATE_ESC) {
		ansi_state = (raw == '[') ? ANSI_STATE_CSI : ANSI_STATE_NORMAL;
		if (ansi_state == ANSI_STATE_CSI) {
			ansi_nparams = 0;
			ansi_params[0] = 0;
		}
		return;
	}

	if (ansi_state == ANSI_STATE_CSI) {
		if (raw >= '0' && raw <= '9') {
			ansi_params[ansi_nparams] =
				ansi_params[ansi_nparams] * 10 + (raw - '0');
		} else if (raw == ';') {
			if (ansi_nparams < ANSI_MAX_PARAMS - 1)
				ansi_params[++ansi_nparams] = 0;
		} else {
			ansi_dispatch_csi(raw);
			ansi_nparams = 0;
			ansi_state = ANSI_STATE_NORMAL;
		}
		return;
	}

	if (raw == '\e') {
		ansi_state = ANSI_STATE_ESC;
		utf8.codepoint = 0;
		utf8.bytes_left = 0;
		return;
	}

	uint32_t cp = utf8_feed(byte);
	if (cp == 0)
		return;

	cursor_erase();

	switch (cp) {
	case '\n':
		newline();
		break;
	case '\r':
		cursor_x = term_x0();
		break;
	case '\t':
		do {
			advance_cursor();
		} while ((cursor_x / _LYRTERM_FONT_WIDTH) % 8);
		break;
	default:
		drawch(cursor_x, cursor_y - _LYRTERM_FONT_ASCENT, cp, current_fg,
			   current_bg);
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

void lyrterm_putcp(uint32_t codepoint)
{
	if (!initialized)
		return;

	cursor_erase();

	switch (codepoint) {
	case '\n':
		newline();
		break;
	case '\r':
		cursor_x = term_x0();
		break;
	case '\t':
		do {
			advance_cursor();
		} while ((cursor_x / _LYRTERM_FONT_WIDTH) % 8);
		break;
	default:
		drawch(cursor_x, cursor_y - _LYRTERM_FONT_ASCENT, codepoint, current_fg,
			   current_bg);
		advance_cursor();
		break;
	}

	cursor_draw();
}

void lyrterm_apply_theme(const lyrterm_theme_t *theme)
{
	if (!theme)
		return;
	active_theme = theme;
	default_fg = theme->fg;
	default_bg = theme->bg;
	current_fg = theme->fg;
	current_bg = theme->bg;
}