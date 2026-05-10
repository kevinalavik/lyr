#include <lib/lyrterm.h>
#include <dev/async.h>
#include <lib/lyrterm_font.h>
#include <lib/string.h>
#include <lib/lyrterm_theme.h>
#include <sync/spinlock.h>
#include <limine.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define _LYRTERM_LINE_PADDING_Y -4
#define _LYRTERM_LINE_HEIGHT \
	(_LYRTERM_FONT_ASCENT + _LYRTERM_FONT_DESCENT + _LYRTERM_LINE_PADDING_Y)

#define _LYRTERM_LINE_PADDING_X 0
#define _LYRTERM_LINE_WIDTH (_LYRTERM_FONT_WIDTH + _LYRTERM_LINE_PADDING_X)

#define _LYRTERM_MARGIN_X 0
#define _LYRTERM_MARGIN_Y 0

#define ANSI_MAX_PARAMS 16
#define LYRTERM_Q_SIZE 8192
#define LYRTERM_DRAIN_BUDGET 256
#define OSC_MAX 256

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

static bool initialized = false;
static uint32_t cols;
static uint32_t rows;
static bool render_enabled = true;
static uint32_t framebuffer_raw_users = 0;

static const lyrterm_theme_t *active_theme = &lyrterm_theme_dark;

static lyrterm_state_t render_state_storage;
static lyrterm_state_t *render_state = &render_state_storage;

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

typedef void (*ansi_handler_t)(int *params, int nparams);

typedef struct {
	char final;
	ansi_handler_t handler;
} ansi_csi_handler_t;

#define cursor_x render_state->cursor_x
#define cursor_y render_state->cursor_y
#define default_fg render_state->default_fg
#define default_bg render_state->default_bg
#define current_fg render_state->current_fg
#define current_bg render_state->current_bg
#define reverse_video render_state->reverse_video
#define ansi_state render_state->ansi_state
#define ansi_params render_state->ansi_params
#define ansi_nparams render_state->ansi_nparams
#define osc_buf render_state->osc_buf
#define osc_len render_state->osc_len
#define osc_saw_esc render_state->osc_saw_esc
#define utf8 render_state->utf8
#define cell_buf render_state->cell_buf

static void cursor_draw(void);

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

static inline uint8_t clamp_u8(int v)
{
	if (v < 0)
		return 0;
	if (v > 255)
		return 255;
	return (uint8_t)v;
}

static inline uint32_t rgb_color(int r, int g, int b)
{
	return pack_color(((uint32_t)clamp_u8(r) << 16) |
					  ((uint32_t)clamp_u8(g) << 8) | ((uint32_t)clamp_u8(b)));
}

static inline void write_pixel(uint32_t x, uint32_t y, uint32_t packed)
{
	if (!render_enabled)
		return;

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

static inline uint32_t effective_fg(void)
{
	return reverse_video ? current_bg : current_fg;
}

static inline uint32_t effective_bg(void)
{
	return reverse_video ? current_fg : current_bg;
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

static inline uint32_t cell_col(void)
{
	return (cursor_x - term_x0()) / _LYRTERM_LINE_WIDTH;
}

static inline uint32_t cell_row(void)
{
	return (cursor_y - term_y0() - _LYRTERM_FONT_ASCENT) / _LYRTERM_LINE_HEIGHT;
}

static void cell_set(uint32_t col, uint32_t row, uint32_t cp, uint32_t fg,
					 uint32_t bg)
{
	if (col >= LYRTERM_MAX_COLS || row >= LYRTERM_MAX_ROWS)
		return;
	cell_buf[row][col].codepoint = cp;
	cell_buf[row][col].fg = fg;
	cell_buf[row][col].bg = bg;
}

static lyrterm_cell_t cell_get(uint32_t col, uint32_t row)
{
	if (col >= LYRTERM_MAX_COLS || row >= LYRTERM_MAX_ROWS)
		return (lyrterm_cell_t){ ' ', 0, 0 };
	return cell_buf[row][col];
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
					  uint32_t packed)
{
	if (!render_enabled)
		return;
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
	if (!render_enabled)
		return;

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

#undef cursor_x
#undef cursor_y
#undef default_fg
#undef default_bg
#undef current_fg
#undef current_bg
#undef reverse_video
#undef ansi_state
#undef ansi_params
#undef ansi_nparams
#undef osc_buf
#undef osc_len
#undef osc_saw_esc
#undef utf8
#undef cell_buf

static void lyrterm_state_reset_internal(lyrterm_state_t *state)
{
	if (!state)
		return;

	memset(state, 0, sizeof(*state));
	state->cursor_x = term_x0();
	state->cursor_y = term_y0() + _LYRTERM_FONT_ASCENT;
	state->default_fg = pack_color(active_theme->fg);
	state->default_bg = pack_color(active_theme->bg);
	state->current_fg = state->default_fg;
	state->current_bg = state->default_bg;
	state->ansi_state = LYRTERM_ANSI_STATE_NORMAL;
}

#define cursor_x render_state->cursor_x
#define cursor_y render_state->cursor_y
#define default_fg render_state->default_fg
#define default_bg render_state->default_bg
#define current_fg render_state->current_fg
#define current_bg render_state->current_bg
#define reverse_video render_state->reverse_video
#define ansi_state render_state->ansi_state
#define ansi_params render_state->ansi_params
#define ansi_nparams render_state->ansi_nparams
#define osc_buf render_state->osc_buf
#define osc_len render_state->osc_len
#define osc_saw_esc render_state->osc_saw_esc
#define utf8 render_state->utf8
#define cell_buf render_state->cell_buf

static void lyrterm_redraw_from_state(void)
{
	if (!initialized)
		return;

	fill_rect(0, 0, fb_width, fb_height, default_bg);

	uint32_t buf_rows = rows < LYRTERM_MAX_ROWS ? rows : LYRTERM_MAX_ROWS;
	uint32_t buf_cols = cols < LYRTERM_MAX_COLS ? cols : LYRTERM_MAX_COLS;

	for (uint32_t row = 0; row < buf_rows; row++) {
		for (uint32_t col = 0; col < buf_cols; col++) {
			lyrterm_cell_t cell = cell_buf[row][col];
			uint32_t fg = cell.fg ? cell.fg : default_fg;
			uint32_t bg = cell.bg ? cell.bg : default_bg;
			uint32_t cp = cell.codepoint ? cell.codepoint : ' ';
			uint32_t x = term_x0() + col * _LYRTERM_LINE_WIDTH;
			uint32_t y = term_y0() + row * _LYRTERM_LINE_HEIGHT;

			drawch(x, y, cp, fg, bg);
		}
	}

	cursor_draw();
}

static void clear_screen(void)
{
	fill_rect(0, 0, fb_width, fb_height, default_bg);
	memset(cell_buf, 0, sizeof(cell_buf));
	cursor_x = term_x0();
	cursor_y = term_y0() + _LYRTERM_FONT_ASCENT;
}

static void clear_line_from_cursor(void)
{
	uint32_t y = cursor_y - _LYRTERM_FONT_ASCENT;

	fill_rect(cursor_x, y, term_x0() + term_width() - cursor_x,
			  _LYRTERM_LINE_HEIGHT, effective_bg());
}

static void cursor_set_pos(uint32_t row, uint32_t col)
{
	if (row == 0)
		row = 1;
	if (col == 0)
		col = 1;

	if (row > rows)
		row = rows;
	if (col > cols)
		col = cols;

	cursor_x = term_x0() + (col - 1) * _LYRTERM_LINE_WIDTH;
	cursor_y =
		term_y0() + (row - 1) * _LYRTERM_LINE_HEIGHT + _LYRTERM_FONT_ASCENT;
}

static void scroll_up(void)
{
	uint32_t buf_rows = rows < LYRTERM_MAX_ROWS ? rows : LYRTERM_MAX_ROWS;
	uint32_t buf_cols = cols < LYRTERM_MAX_COLS ? cols : LYRTERM_MAX_COLS;

	if (buf_rows == 0 || buf_cols == 0)
		return;

	if (render_enabled) {
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

	/* Scroll cell buffer up one row and clear the last row. */
	for (uint32_t r = 0; r + 1 < buf_rows; r++)
		memcpy(cell_buf[r], cell_buf[r + 1], buf_cols * sizeof(lyrterm_cell_t));
	memset(cell_buf[buf_rows - 1], 0, buf_cols * sizeof(lyrterm_cell_t));
}

static void cursor_draw(void)
{
	uint32_t col = cell_col();
	uint32_t row = cell_row();
	lyrterm_cell_t cell = cell_get(col, row);

	uint32_t fg = cell.bg ? cell.bg : default_bg;
	uint32_t bg = cell.fg ? cell.fg : default_fg;

	/* If the cell is blank, fall back to a solid inverted block. */
	uint32_t cp = cell.codepoint ? cell.codepoint : ' ';

#ifdef LYRTERM_LINE_CURSOR
	uint32_t top = cursor_y - _LYRTERM_FONT_ASCENT;
	uint32_t h = cursor_draw_height();
	uint32_t y = top + (_LYRTERM_LINE_HEIGHT - h);
	fill_rect(cursor_x, y, _LYRTERM_FONT_WIDTH, h, bg);
#else
	drawch(cursor_x, cursor_y - _LYRTERM_FONT_ASCENT, cp, fg, bg);
#endif
}

static void cursor_erase(void)
{
	uint32_t col = cell_col();
	uint32_t row = cell_row();
	lyrterm_cell_t cell = cell_get(col, row);

	uint32_t fg = cell.fg ? cell.fg : default_fg;
	uint32_t bg = cell.bg ? cell.bg : default_bg;
	uint32_t cp = cell.codepoint ? cell.codepoint : ' ';

	drawch(cursor_x, cursor_y - _LYRTERM_FONT_ASCENT, cp, fg, bg);
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
		reverse_video = false;
		return;
	}

	for (int i = 0; i < nparams; i++) {
		int p = params[i];

		if (p == 0) {
			current_fg = default_fg;
			current_bg = default_bg;
			reverse_video = false;
		} else if (p == 7) {
			reverse_video = true;
		} else if (p == 27) {
			reverse_video = false;
		} else if (p == 39) {
			current_fg = default_fg;
		} else if (p == 49) {
			current_bg = default_bg;
		} else if (p >= 30 && p <= 37) {
			current_fg = pack_color(ansi_colors[p - 30]);
		} else if (p >= 40 && p <= 47) {
			current_bg = pack_color(ansi_colors[p - 40]);
		} else if (p >= 90 && p <= 97) {
			current_fg = pack_color(ansi_colors_bright[p - 90]);
		} else if (p >= 100 && p <= 107) {
			current_bg = pack_color(ansi_colors_bright[p - 100]);
		} else if ((p == 38 || p == 48) && i + 1 < nparams) {
			bool is_fg = p == 38;
			int mode = params[++i];

			if (mode == 2 && i + 3 < nparams) {
				int r = params[++i];
				int g = params[++i];
				int b = params[++i];

				if (is_fg)
					current_fg = rgb_color(r, g, b);
				else
					current_bg = rgb_color(r, g, b);
			}
		}
	}
}

static void ansi_handle_ed(int *params, int nparams)
{
	int mode = nparams ? params[0] : 0;

	/* ESC[J or ESC[0J: clear from cursor to end.
	   ESC[2J: clear whole screen. */
	if (mode == 2) {
		clear_screen();
	} else {
		uint32_t y = cursor_y - _LYRTERM_FONT_ASCENT;

		fill_rect(cursor_x, y, term_x0() + term_width() - cursor_x,
				  _LYRTERM_LINE_HEIGHT, effective_bg());

		if (y + _LYRTERM_LINE_HEIGHT < term_y0() + term_height()) {
			fill_rect(term_x0(), y + _LYRTERM_LINE_HEIGHT, term_width(),
					  term_y0() + term_height() - (y + _LYRTERM_LINE_HEIGHT),
					  effective_bg());
		}
	}
}

static void ansi_handle_el(int *params, int nparams)
{
	int mode = nparams ? params[0] : 0;

	if (mode == 2) {
		uint32_t y = cursor_y - _LYRTERM_FONT_ASCENT;
		fill_rect(term_x0(), y, term_width(), _LYRTERM_LINE_HEIGHT,
				  effective_bg());
	} else {
		clear_line_from_cursor();
	}
}

static void ansi_handle_cup(int *params, int nparams)
{
	uint32_t row = 1;
	uint32_t col = 1;

	if (nparams >= 1 && params[0])
		row = (uint32_t)params[0];
	if (nparams >= 2 && params[1])
		col = (uint32_t)params[1];

	cursor_set_pos(row, col);
}

static void ansi_handle_cuu(int *params, int nparams)
{
	uint32_t n = (nparams && params[0]) ? (uint32_t)params[0] : 1;
	uint32_t row =
		(cursor_y - term_y0() - _LYRTERM_FONT_ASCENT) / _LYRTERM_LINE_HEIGHT;

	if (n > row)
		row = 0;
	else
		row -= n;

	cursor_set_pos(row + 1, ((cursor_x - term_x0()) / _LYRTERM_LINE_WIDTH) + 1);
}

static void ansi_handle_cud(int *params, int nparams)
{
	uint32_t n = (nparams && params[0]) ? (uint32_t)params[0] : 1;
	uint32_t row =
		(cursor_y - term_y0() - _LYRTERM_FONT_ASCENT) / _LYRTERM_LINE_HEIGHT;

	row += n;
	if (row >= rows)
		row = rows - 1;

	cursor_set_pos(row + 1, ((cursor_x - term_x0()) / _LYRTERM_LINE_WIDTH) + 1);
}

static void ansi_handle_cuf(int *params, int nparams)
{
	uint32_t n = (nparams && params[0]) ? (uint32_t)params[0] : 1;
	uint32_t col = (cursor_x - term_x0()) / _LYRTERM_LINE_WIDTH;

	col += n;
	if (col >= cols)
		col = cols - 1;

	cursor_set_pos(
		((cursor_y - term_y0() - _LYRTERM_FONT_ASCENT) / _LYRTERM_LINE_HEIGHT) +
			1,
		col + 1);
}

static void ansi_handle_cub(int *params, int nparams)
{
	uint32_t n = (nparams && params[0]) ? (uint32_t)params[0] : 1;
	uint32_t col = (cursor_x - term_x0()) / _LYRTERM_LINE_WIDTH;

	if (n > col)
		col = 0;
	else
		col -= n;

	cursor_set_pos(
		((cursor_y - term_y0() - _LYRTERM_FONT_ASCENT) / _LYRTERM_LINE_HEIGHT) +
			1,
		col + 1);
}

static const ansi_csi_handler_t ansi_csi_handlers[] = {
	{ 'm', ansi_handle_sgr }, { 'J', ansi_handle_ed },
	{ 'K', ansi_handle_el },  { 'H', ansi_handle_cup },
	{ 'f', ansi_handle_cup }, { 'A', ansi_handle_cuu },
	{ 'B', ansi_handle_cud }, { 'C', ansi_handle_cuf },
	{ 'D', ansi_handle_cub },
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

/*
 * OSC dispatcher.
 *
 * The buffer contains everything between "ESC ]" and the terminator
 * (BEL or ESC \).  Format is "Ps;text" where Ps is a numeric parameter.
 *
 * Supported sequences:
 *   OSC 0 ; text ST  — set icon name and window title (text ignored)
 *   OSC 1 ; text ST  — set icon name               (text ignored)
 *   OSC 2 ; text ST  — set window title             (text ignored)
 *
 * All are silently consumed; lyrterm is a framebuffer terminal with no
 * window manager, so the title string has nowhere to go.  Consuming the
 * sequence cleanly prevents the raw bytes from appearing on screen.
 */
static void ansi_dispatch_osc(const char *buf, size_t len)
{
	(void)buf;
	(void)len;
	/* No-op: sequence consumed, nothing rendered. */
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

	initialized = true;
	lyrterm_state_reset_internal(render_state);
	fill_rect(0, 0, fb_width, fb_height, default_bg);

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
	reverse_video = false;
}

void lyrterm_set_colors(uint32_t fg, uint32_t bg)
{
	default_fg = pack_color(fg);
	default_bg = pack_color(bg);
	current_fg = default_fg;
	current_bg = default_bg;
	reverse_video = false;
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
	case '\b':
		if (cursor_x > term_x0())
			cursor_x -= _LYRTERM_LINE_WIDTH;
		break;
	default:
		if (codepoint < 0x20)
			break;
		uint32_t fg = effective_fg();
		uint32_t bg = effective_bg();

		drawch(cursor_x, cursor_y - _LYRTERM_FONT_ASCENT, codepoint, fg, bg);
		cell_set(cell_col(), cell_row(), codepoint, fg, bg);
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

	/* ------------------------------------------------------------------ */
	/* OSC state: accumulate until BEL (0x07) or ST (ESC \)               */
	/* ------------------------------------------------------------------ */
	if (ansi_state == LYRTERM_ANSI_STATE_OSC) {
		if (osc_saw_esc) {
			osc_saw_esc = false;
			if (raw == '\\') {
				/* ESC \ — String Terminator */
				osc_buf[osc_len] = '\0';
				ansi_dispatch_osc(osc_buf, osc_len);
				ansi_state = LYRTERM_ANSI_STATE_NORMAL;
			} else {
				/*
				 * The ESC was not followed by '\'; treat it as an
				 * abort and re-process the current byte normally.
				 */
				ansi_state = LYRTERM_ANSI_STATE_NORMAL;
				lyrterm_putch_locked(raw);
			}
			return;
		}

		if (raw == '\007') {
			/* BEL — terminates OSC */
			osc_buf[osc_len] = '\0';
			ansi_dispatch_osc(osc_buf, osc_len);
			ansi_state = LYRTERM_ANSI_STATE_NORMAL;
		} else if (raw == '\033') {
			/* Possible start of ESC \ */
			osc_saw_esc = true;
		} else {
			if (osc_len + 1 < OSC_MAX)
				osc_buf[osc_len++] = raw;
		}
		return;
	}

	/* ------------------------------------------------------------------ */
	/* ESC state                                                           */
	/* ------------------------------------------------------------------ */
	if (ansi_state == LYRTERM_ANSI_STATE_ESC) {
		if (raw == '[') {
			ansi_state = LYRTERM_ANSI_STATE_CSI;
			ansi_nparams = 0;
			ansi_params[0] = 0;
		} else if (raw == ']') {
			ansi_state = LYRTERM_ANSI_STATE_OSC;
			osc_len = 0;
			osc_saw_esc = false;
			osc_buf[0] = '\0';
		} else {
			ansi_state = LYRTERM_ANSI_STATE_NORMAL;
		}
		return;
	}

	/* ------------------------------------------------------------------ */
	/* CSI state                                                           */
	/* ------------------------------------------------------------------ */
	if (ansi_state == LYRTERM_ANSI_STATE_CSI) {
		if (raw >= '0' && raw <= '9') {
			ansi_params[ansi_nparams] =
				ansi_params[ansi_nparams] * 10 + (raw - '0');
		} else if (raw == ';') {
			if (ansi_nparams < ANSI_MAX_PARAMS - 1)
				ansi_params[++ansi_nparams] = 0;
		} else {
			ansi_dispatch_csi(raw);
			ansi_nparams = 0;
			ansi_state = LYRTERM_ANSI_STATE_NORMAL;
		}

		return;
	}

	/* ------------------------------------------------------------------ */
	/* Normal state                                                        */
	/* ------------------------------------------------------------------ */
	if (raw == '\033') {
		ansi_state = LYRTERM_ANSI_STATE_ESC;
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
	if (!initialized)
		return;

	spinlock_acquire(&lyrterm_render_lock);

	for (;;) {
		spinlock_acquire(&lyrterm_lock);
		if (lyrterm_q_empty()) {
			spinlock_release(&lyrterm_lock);
			break;
		}

		char ch = lyrterm_q[lyrterm_rpos];
		lyrterm_rpos = (lyrterm_rpos + 1) % LYRTERM_Q_SIZE;
		spinlock_release(&lyrterm_lock);

		lyrterm_putch_locked(ch);
	}

	spinlock_release(&lyrterm_render_lock);
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

void lyrterm_get_size(uint32_t *cols_out, uint32_t *rows_out,
					  uint32_t *width_out, uint32_t *height_out)
{
	if (cols_out)
		*cols_out = initialized ? cols : 0;

	if (rows_out)
		*rows_out = initialized ? rows : 0;

	if (width_out)
		*width_out = initialized ? fb_width : 0;

	if (height_out)
		*height_out = initialized ? fb_height : 0;
}

int lyrterm_get_framebuffer_info(lyrterm_framebuffer_info_t *out)
{
	if (!out)
		return -EINVAL;
	if (!initialized)
		return -ENODEV;

	out->address = (uint64_t)fb_base;
	out->width = fb_width;
	out->height = fb_height;
	out->pitch = fb_pitch;
	out->bpp = fb_bpp;
	out->size = fb_pitch * fb_height;
	return 0;
}


void lyrterm_framebuffer_acquire(void)
{
	if (!initialized)
		return;

	spinlock_acquire(&lyrterm_render_lock);
	/*
	 * This is deliberately idempotent. devfs currently has no open callback,
	 * so fbdev_write() calls acquire() before each write. Treat any writer as
	 * the raw framebuffer owner until the file is closed.
	 */
	framebuffer_raw_users = 1;
	render_enabled = false;
	spinlock_release(&lyrterm_render_lock);
}

void lyrterm_framebuffer_release(void)
{
	if (!initialized)
		return;

	spinlock_acquire(&lyrterm_render_lock);
	if (framebuffer_raw_users != 0) {
		framebuffer_raw_users = 0;
		render_enabled = true;
		lyrterm_redraw_from_state();
	}
	spinlock_release(&lyrterm_render_lock);
}

int lyrterm_framebuffer_read(uint64_t off, void *buf, size_t len, size_t *done)
{
	uint32_t size = fb_pitch * fb_height;

	if (done)
		*done = 0;
	if (!buf)
		return -EINVAL;
	if (!initialized)
		return -ENODEV;
	if (off >= size)
		return 0;

	size_t avail = (size_t)(size - off);
	if (len > avail)
		len = avail;

	spinlock_acquire(&lyrterm_render_lock);
	memcpy(buf, fb_base + off, len);
	spinlock_release(&lyrterm_render_lock);

	if (done)
		*done = len;
	return 0;
}

int lyrterm_framebuffer_write(uint64_t off, const void *buf, size_t len,
							  size_t *done)
{
	uint32_t size = fb_pitch * fb_height;

	if (done)
		*done = 0;
	if (!buf)
		return -EINVAL;
	if (!initialized)
		return -ENODEV;
	if (off >= size)
		return 0;

	size_t avail = (size_t)(size - off);
	if (len > avail)
		len = avail;

	spinlock_acquire(&lyrterm_render_lock);
	memcpy(fb_base + off, buf, len);
	spinlock_release(&lyrterm_render_lock);

	if (done)
		*done = len;
	return 0;
}

void lyrterm_capture_state(lyrterm_state_t *out)
{
	if (!out)
		return;

	spinlock_acquire(&lyrterm_render_lock);
	memcpy(out, render_state, sizeof(*out));
	spinlock_release(&lyrterm_render_lock);
}

void lyrterm_restore_state(const lyrterm_state_t *in)
{
	if (!in)
		return;

	spinlock_acquire(&lyrterm_render_lock);
	memcpy(render_state, in, sizeof(*render_state));
	if (framebuffer_raw_users == 0) {
		render_enabled = true;
		lyrterm_redraw_from_state();
	}
	spinlock_release(&lyrterm_render_lock);
}

void lyrterm_update_state(lyrterm_state_t *state, const char *buf, size_t len)
{
	if (!state || !buf || len == 0)
		return;

	spinlock_acquire(&lyrterm_render_lock);

	bool saved_render_enabled = render_enabled;
	lyrterm_state_t *saved_state = render_state;
	render_state = state;
	render_enabled = false;

	for (size_t i = 0; i < len; i++)
		lyrterm_putch_locked(buf[i]);

	render_state = saved_state;
	render_enabled = saved_render_enabled;

	spinlock_release(&lyrterm_render_lock);
}
