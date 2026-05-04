#include <lib/lyrterm.h>
#include <dev/async.h>
#include <lib/lyrterm_font.h>
#include <lib/string.h>
#include <lib/lyrterm_theme.h>
#include <sync/spinlock.h>
#include <limine.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define _LYRTERM_LINE_PADDING_Y -3
#define _LYRTERM_LINE_HEIGHT \
	(_LYRTERM_FONT_ASCENT + _LYRTERM_FONT_DESCENT + _LYRTERM_LINE_PADDING_Y)

#define _LYRTERM_LINE_PADDING_X 0
#define _LYRTERM_LINE_WIDTH (_LYRTERM_FONT_WIDTH + _LYRTERM_LINE_PADDING_X)

#define _LYRTERM_MARGIN_X 10
#define _LYRTERM_MARGIN_Y 10

#define ANSI_MAX_PARAMS 8
#define LYRTERM_Q_SIZE 8192
#define LYRTERM_DRAIN_BUDGET 256

static uint8_t *fb_base;
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t fb_pitch;
static uint32_t fb_bpp;
static uint32_t fb_Bpp;
static uint8_t fb_memory_model;

static uint8_t red_mask_size, red_mask_shift;
static uint8_t green_mask_size, green_mask_shift;
static uint8_t blue_mask_size, blue_mask_shift;

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

static spinlock_t lyrterm_lock = SPINLOCK_INIT;
static spinlock_t lyrterm_render_lock = SPINLOCK_INIT;
static char lyrterm_q[LYRTERM_Q_SIZE];
static size_t lyrterm_rpos;
static size_t lyrterm_wpos;
static size_t lyrterm_dropped;

#define ansi_colors (active_theme->ansi_normal)
#define ansi_colors_bright (active_theme->ansi_bright)

#define CURSOR_COLOR current_fg

#ifdef LYRTERM_LINE_CURSOR
#define CURSOR_HEIGHT ((_LYRTERM_FONT_HEIGHT - _LYRTERM_LINE_PADDING_Y) / 6)
#else
#define CURSOR_HEIGHT _LYRTERM_FONT_HEIGHT
#endif

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

static inline uint32_t min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static inline uint32_t glyph_draw_height(void)
{
	return min_u32(_LYRTERM_FONT_HEIGHT, _LYRTERM_LINE_HEIGHT);
}

static inline uint32_t cursor_draw_height(void)
{
	return min_u32(CURSOR_HEIGHT, _LYRTERM_LINE_HEIGHT);
}

static bool lyrterm_q_empty(void)
{
	return lyrterm_rpos == lyrterm_wpos;
}

static bool lyrterm_q_full(void)
{
	return ((lyrterm_wpos + 1) % LYRTERM_Q_SIZE) == lyrterm_rpos;
}

static void lyrterm_enqueue_locked(const char *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (lyrterm_q_full()) {
			lyrterm_dropped += len - i;
			break;
		}

		lyrterm_q[lyrterm_wpos] = buf[i];
		lyrterm_wpos = (lyrterm_wpos + 1) % LYRTERM_Q_SIZE;
	}
}

static inline uint32_t pack_color(uint32_t rgb)
{
	uint8_t r8 = (rgb >> 16) & 0xFF;
	uint8_t g8 = (rgb >> 8) & 0xFF;
	uint8_t b8 = rgb & 0xFF;

	uint32_t r = (uint32_t)r8 >> (8 - red_mask_size);
	uint32_t g = (uint32_t)g8 >> (8 - green_mask_size);
	uint32_t b = (uint32_t)b8 >> (8 - blue_mask_size);

	return (r << red_mask_shift) | (g << green_mask_shift) |
		   (b << blue_mask_shift);
}

static inline void write_pixel(uint32_t x, uint32_t y, uint32_t packed)
{
	uint8_t *p = fb_base + (uint32_t)(y * fb_pitch + x * fb_Bpp);

	switch (fb_Bpp) {
	case 2:
		p[0] = (uint8_t)(packed & 0xFF);
		p[1] = (uint8_t)(packed >> 8);
		break;
	case 3:
		p[0] = (uint8_t)(packed & 0xFF);
		p[1] = (uint8_t)((packed >> 8) & 0xFF);
		p[2] = (uint8_t)((packed >> 16) & 0xFF);
		break;
	case 4:
	default:
		*((uint32_t *)p) = packed;
		break;
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

static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
					  uint32_t packed)
{
	if (w == 0 || h == 0)
		return;

	if (fb_Bpp == 4) {
		for (uint32_t row = 0; row < h; row++) {
			uint32_t *p = (uint32_t *)(fb_base + (y + row) * fb_pitch + x * 4);
			uint32_t n = w;

			__asm__ volatile("rep stosl"
							 : "+D"(p), "+c"(n)
							 : "a"(packed)
							 : "memory");
		}
	} else {
		for (uint32_t row = 0; row < h; row++) {
			for (uint32_t col = 0; col < w; col++)
				write_pixel(x + col, y + row, packed);
		}
	}
}

static void drawch(uint32_t x, uint32_t y, uint32_t codepoint, uint32_t fg,
				   uint32_t bg)
{
	int glyph_index = _lyrterm_find_glyph(codepoint);
	const uint8_t *glyph = (glyph_index >= 0) ? _lyrterm_font[glyph_index] :
												_lyrterm_font_sentinel;

	const uint32_t bytes_per_row = (_LYRTERM_FONT_WIDTH + 7) / 8;
	const uint32_t draw_h = glyph_draw_height();

	if (fb_Bpp == 4) {
		for (uint32_t row = 0; row < draw_h; row++) {
			const uint8_t *row_data = &glyph[row * bytes_per_row];
			uint32_t *dst =
				(uint32_t *)(fb_base + (y + row) * fb_pitch + x * 4);

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
	} else {
		for (uint32_t row = 0; row < draw_h; row++) {
			const uint8_t *row_data = &glyph[row * bytes_per_row];
			uint32_t col = 0;

			for (uint32_t b = 0; b < bytes_per_row; b++) {
				uint8_t byte = row_data[b];
				uint32_t remaining = _LYRTERM_FONT_WIDTH - col;
				uint32_t bits = remaining < 8 ? remaining : 8;

				for (uint32_t bit = 0; bit < bits; bit++) {
					write_pixel(x + col, y + row, (byte & 0x80) ? fg : bg);
					col++;
					byte <<= 1;
				}
			}
		}
	}

	if (_LYRTERM_LINE_HEIGHT > _LYRTERM_FONT_HEIGHT) {
		fill_rect(x, y + _LYRTERM_FONT_HEIGHT, _LYRTERM_FONT_WIDTH,
				  _LYRTERM_LINE_HEIGHT - _LYRTERM_FONT_HEIGHT, bg);
	}
}

static void scroll_up(void)
{
	uint32_t x0 = term_x0();
	uint32_t y0 = term_y0();
	uint32_t w = term_width();
	uint32_t h = term_height();

	if (h <= _LYRTERM_LINE_HEIGHT)
		return;

	uint32_t rows_to_copy = h - _LYRTERM_LINE_HEIGHT;

	if (x0 == 0 && w == fb_width) {
		uint8_t *dst = fb_base + y0 * fb_pitch;
		uint8_t *src = fb_base + (y0 + _LYRTERM_LINE_HEIGHT) * fb_pitch;

		memcpy(dst, src, rows_to_copy * fb_pitch);
	} else {
		for (uint32_t row = 0; row < rows_to_copy; row++) {
			uint8_t *dst = fb_base + (y0 + row) * fb_pitch + x0 * fb_Bpp;
			uint8_t *src = fb_base +
						   (y0 + row + _LYRTERM_LINE_HEIGHT) * fb_pitch +
						   x0 * fb_Bpp;

			memcpy(dst, src, w * fb_Bpp);
		}
	}

	fill_rect(x0, y0 + h - _LYRTERM_LINE_HEIGHT, w, _LYRTERM_LINE_HEIGHT,
			  default_bg);
}

static void cursor_draw(void)
{
	uint32_t top = cursor_y - _LYRTERM_FONT_ASCENT;
	uint32_t h = cursor_draw_height();
	uint32_t y = top + (_LYRTERM_LINE_HEIGHT - h);

	fill_rect(cursor_x, y, _LYRTERM_FONT_WIDTH, h, CURSOR_COLOR);
}

static void cursor_erase(void)
{
	uint32_t top = cursor_y - _LYRTERM_FONT_ASCENT;
	uint32_t h = cursor_draw_height();
	uint32_t y = top + (_LYRTERM_LINE_HEIGHT - h);

	fill_rect(cursor_x, y, _LYRTERM_FONT_WIDTH, h, current_bg);
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
	cursor_x += _LYRTERM_LINE_WIDTH;

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
			current_fg = pack_color(ansi_colors[p - 30]);
		} else if (p >= 40 && p <= 47) {
			current_bg = pack_color(ansi_colors[p - 40]);
		} else if (p >= 90 && p <= 97) {
			current_fg = pack_color(ansi_colors_bright[p - 90]);
		} else if (p >= 100 && p <= 107) {
			current_bg = pack_color(ansi_colors_bright[p - 100]);
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

	if (--utf8.bytes_left == 0) {
		uint32_t cp = utf8.codepoint;
		utf8.codepoint = 0;

		if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
			return 0xFFFD;

		return cp;
	}

	return 0;
}

static void lyrterm_putch_locked(char raw);
static void lyrterm_putcp_locked(uint32_t codepoint);

static size_t lyrterm_async_drain(size_t budget, void *context)
{
	(void)context;
	return lyrterm_drain(budget);
}

void lyrterm_init(const struct limine_framebuffer *lfb)
{
	if (!lfb || !lfb->address || !lfb->width || !lfb->height || !lfb->pitch)
		return;

	fb_base = (uint8_t *)lfb->address;
	fb_width = (uint32_t)lfb->width;
	fb_height = (uint32_t)lfb->height;
	fb_pitch = (uint32_t)lfb->pitch;
	fb_bpp = lfb->bpp;
	fb_Bpp = (lfb->bpp + 7) / 8;
	fb_memory_model = lfb->memory_model;

	red_mask_size = lfb->red_mask_size;
	red_mask_shift = lfb->red_mask_shift;
	green_mask_size = lfb->green_mask_size;
	green_mask_shift = lfb->green_mask_shift;
	blue_mask_size = lfb->blue_mask_size;
	blue_mask_shift = lfb->blue_mask_shift;

	cursor_x = term_x0();
	cursor_y = term_y0() + _LYRTERM_FONT_ASCENT;

	cols = term_width() / _LYRTERM_LINE_WIDTH;
	rows = term_height() / _LYRTERM_LINE_HEIGHT;

	utf8.codepoint = 0;
	utf8.bytes_left = 0;

	lyrterm_apply_theme(active_theme);
	fill_rect(0, 0, fb_width, fb_height, default_bg);

	initialized = true;

	async_io_register_drain_hook(lyrterm_async_drain, NULL);
	cursor_draw();
}

void lyrterm_apply_theme(const lyrterm_theme_t *theme)
{
	if (!theme)
		return;

	active_theme = theme;
	default_fg = pack_color(theme->fg);
	default_bg = pack_color(theme->bg);
	current_fg = default_fg;
	current_bg = default_bg;
}

void lyrterm_set_colors(uint32_t fg, uint32_t bg)
{
	default_fg = pack_color(fg);
	default_bg = pack_color(bg);
	current_fg = default_fg;
	current_bg = default_bg;
}

static void tab_advance(void)
{
	do {
		advance_cursor();
	} while ((((cursor_x - term_x0()) / _LYRTERM_LINE_WIDTH) % 8) != 0);
}

static void lyrterm_putcp_raw_locked(uint32_t codepoint)
{
	cursor_erase();

	switch (codepoint) {
	case '\n':
		newline();
		break;
	case '\r':
		cursor_x = term_x0();
		break;
	case '\t':
		tab_advance();
		break;
	default:
		drawch(cursor_x, cursor_y - _LYRTERM_FONT_ASCENT, codepoint, current_fg,
			   current_bg);
		advance_cursor();
		break;
	}

	cursor_draw();
}

static void lyrterm_putch_locked(char raw)
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

	lyrterm_putcp_raw_locked(cp);
}

void lyrterm_putch(char raw)
{
	if (!spinlock_try_acquire(&lyrterm_render_lock))
		return;

	lyrterm_putch_locked(raw);

	spinlock_release(&lyrterm_render_lock);
}

void lyrterm_putstr(const char *str)
{
	if (!str)
		return;

	spinlock_acquire(&lyrterm_render_lock);

	while (*str)
		lyrterm_putch_locked(*str++);

	spinlock_release(&lyrterm_render_lock);
}

void lyrterm_wbuf(const char *buf, size_t len)
{
	if (!buf || len == 0)
		return;

	spinlock_acquire(&lyrterm_lock);
	lyrterm_enqueue_locked(buf, len);
	spinlock_release(&lyrterm_lock);
}

size_t lyrterm_drain(size_t budget)
{
	size_t drained = 0;

	if (!initialized)
		return 0;

	if (!spinlock_try_acquire(&lyrterm_render_lock))
		return 0;

	while (budget == 0 || drained < budget) {
		char ch;

		if (!spinlock_try_acquire(&lyrterm_lock))
			break;

		if (lyrterm_q_empty()) {
			spinlock_release(&lyrterm_lock);
			break;
		}

		ch = lyrterm_q[lyrterm_rpos];
		lyrterm_rpos = (lyrterm_rpos + 1) % LYRTERM_Q_SIZE;

		spinlock_release(&lyrterm_lock);

		lyrterm_putch_locked(ch);
		drained++;
	}

	spinlock_release(&lyrterm_render_lock);

	return drained;
}

void lyrterm_flush(void)
{
	for (;;) {
		bool checked_empty = false;
		bool empty = false;
		size_t drained = lyrterm_drain(LYRTERM_DRAIN_BUDGET);

		if (spinlock_try_acquire(&lyrterm_lock)) {
			empty = lyrterm_q_empty();
			checked_empty = true;
			spinlock_release(&lyrterm_lock);
		}

		if (checked_empty && empty)
			break;

		if (drained == 0)
			break;
	}
}

size_t lyrterm_dropped_bytes(void)
{
	size_t dropped = 0;

	if (spinlock_try_acquire(&lyrterm_lock)) {
		dropped = lyrterm_dropped;
		spinlock_release(&lyrterm_lock);
	}

	return dropped;
}

static void lyrterm_putcp_locked(uint32_t codepoint)
{
	if (!initialized)
		return;

	lyrterm_putcp_raw_locked(codepoint);
}

void lyrterm_putcp(uint32_t codepoint)
{
	if (!spinlock_try_acquire(&lyrterm_render_lock))
		return;

	lyrterm_putcp_locked(codepoint);

	spinlock_release(&lyrterm_render_lock);
}