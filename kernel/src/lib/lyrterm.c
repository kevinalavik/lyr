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

#define _LYRTERM_MARGIN_X 10
#define _LYRTERM_MARGIN_Y 10

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

static bool cursor_visible = true;
static uint32_t saved_cursor_x;
static uint32_t saved_cursor_y;

static uint32_t scroll_top = 0;
static uint32_t scroll_bottom = 0;

static bool csi_private = false;
static bool csi_private_qmark = false;
static bool esc_charset_skip = false;

static bool bold_attr = false;

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
static void cursor_erase(void);
static void cursor_set_pos(uint32_t row, uint32_t col);
static void scroll_up_region(uint32_t top, uint32_t bottom);
static void scroll_down_region(uint32_t top, uint32_t bottom);
static void newline(void);

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

static uint32_t max_visible_cols(void)
{
	return cols < LYRTERM_MAX_COLS ? cols : LYRTERM_MAX_COLS;
}

static uint32_t max_visible_rows(void)
{
	return rows < LYRTERM_MAX_ROWS ? rows : LYRTERM_MAX_ROWS;
}

static uint32_t current_col(void)
{
	return cell_col();
}

static uint32_t current_row(void)
{
	return cell_row();
}

static void cell_clear(uint32_t col, uint32_t row)
{
	if (col >= LYRTERM_MAX_COLS || row >= LYRTERM_MAX_ROWS)
		return;

	cell_buf[row][col] = (lyrterm_cell_t){ 0, 0, 0 };
}

static void cell_clear_range(uint32_t start_col, uint32_t start_row,
							 uint32_t end_col, uint32_t end_row)
{
	uint32_t max_rows = max_visible_rows();
	uint32_t max_cols = max_visible_cols();

	if (max_rows == 0 || max_cols == 0)
		return;

	if (start_row >= max_rows)
		return;
	if (end_row >= max_rows)
		end_row = max_rows - 1;

	for (uint32_t row = start_row; row <= end_row; row++) {
		uint32_t c0 = row == start_row ? start_col : 0;
		uint32_t c1 = row == end_row ? end_col : max_cols - 1;

		if (c0 >= max_cols)
			continue;
		if (c1 >= max_cols)
			c1 = max_cols - 1;

		for (uint32_t col = c0; col <= c1; col++)
			cell_clear(col, row);
	}
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

static void redraw_cell(uint32_t col, uint32_t row)
{
	if (col >= max_visible_cols() || row >= max_visible_rows())
		return;

	lyrterm_cell_t cell = cell_get(col, row);
	uint32_t fg = cell.fg ? cell.fg : default_fg;
	uint32_t bg = cell.bg ? cell.bg : default_bg;
	uint32_t cp = cell.codepoint ? cell.codepoint : ' ';

	uint32_t x = term_x0() + col * _LYRTERM_LINE_WIDTH;
	uint32_t y = term_y0() + row * _LYRTERM_LINE_HEIGHT;

	drawch(x, y, cp, fg, bg);
}

static void redraw_line(uint32_t row)
{
	if (row >= max_visible_rows())
		return;

	for (uint32_t col = 0; col < max_visible_cols(); col++)
		redraw_cell(col, row);
}

static void redraw_region(uint32_t top, uint32_t bottom)
{
	uint32_t max_rows = max_visible_rows();

	if (max_rows == 0)
		return;

	if (top >= max_rows)
		return;
	if (bottom >= max_rows)
		bottom = max_rows - 1;

	for (uint32_t row = top; row <= bottom; row++)
		redraw_line(row);
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

	uint32_t buf_rows = max_visible_rows();
	uint32_t buf_cols = max_visible_cols();

	for (uint32_t row = 0; row < buf_rows; row++) {
		for (uint32_t col = 0; col < buf_cols; col++)
			redraw_cell(col, row);
	}

	cursor_draw();
}

static void clear_screen(void)
{
	if (cursor_visible)
		cursor_erase();

	fill_rect(0, 0, fb_width, fb_height, default_bg);
	memset(cell_buf, 0, sizeof(cell_buf));
	cursor_x = term_x0();
	cursor_y = term_y0() + _LYRTERM_FONT_ASCENT;
}

static void clear_line_from_cursor(void)
{
	uint32_t y = cursor_y - _LYRTERM_FONT_ASCENT;
	uint32_t col = cell_col();
	uint32_t row = cell_row();

	fill_rect(cursor_x, y, term_x0() + term_width() - cursor_x,
			  _LYRTERM_LINE_HEIGHT, effective_bg());

	if (row < rows)
		cell_clear_range(col, row, cols - 1, row);
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

static void cursor_set_col(uint32_t col)
{
	if (col == 0)
		col = 1;
	if (col > cols)
		col = cols;

	cursor_x = term_x0() + (col - 1) * _LYRTERM_LINE_WIDTH;
}

static void cursor_set_row(uint32_t row)
{
	if (row == 0)
		row = 1;
	if (row > rows)
		row = rows;

	cursor_y =
		term_y0() + (row - 1) * _LYRTERM_LINE_HEIGHT + _LYRTERM_FONT_ASCENT;
}

static void cursor_save(void)
{
	saved_cursor_x = cursor_x;
	saved_cursor_y = cursor_y;
}

static void cursor_restore(void)
{
	cursor_x = saved_cursor_x;
	cursor_y = saved_cursor_y;

	if (cursor_x < term_x0())
		cursor_x = term_x0();
	if (cursor_y < term_y0() + _LYRTERM_FONT_ASCENT)
		cursor_y = term_y0() + _LYRTERM_FONT_ASCENT;

	if (cols != 0 && cursor_x >= term_x0() + term_width())
		cursor_x = term_x0() + (cols - 1) * _LYRTERM_LINE_WIDTH;
	if (rows != 0 && cursor_y >= term_y0() + term_height())
		cursor_y = term_y0() + (rows - 1) * _LYRTERM_LINE_HEIGHT +
				   _LYRTERM_FONT_ASCENT;
}

static void scroll_up_region(uint32_t top, uint32_t bottom)
{
	uint32_t max_rows = max_visible_rows();
	uint32_t max_cols = max_visible_cols();

	if (max_rows == 0 || max_cols == 0)
		return;

	if (top >= max_rows)
		return;
	if (bottom >= max_rows)
		bottom = max_rows - 1;
	if (top >= bottom)
		return;

	for (uint32_t r = top; r < bottom; r++)
		memcpy(cell_buf[r], cell_buf[r + 1], max_cols * sizeof(lyrterm_cell_t));

	memset(cell_buf[bottom], 0, max_cols * sizeof(lyrterm_cell_t));

	redraw_region(top, bottom);
}

static void scroll_down_region(uint32_t top, uint32_t bottom)
{
	uint32_t max_rows = max_visible_rows();
	uint32_t max_cols = max_visible_cols();

	if (max_rows == 0 || max_cols == 0)
		return;

	if (top >= max_rows)
		return;
	if (bottom >= max_rows)
		bottom = max_rows - 1;
	if (top >= bottom)
		return;

	for (uint32_t r = bottom; r > top; r--)
		memcpy(cell_buf[r], cell_buf[r - 1], max_cols * sizeof(lyrterm_cell_t));

	memset(cell_buf[top], 0, max_cols * sizeof(lyrterm_cell_t));

	redraw_region(top, bottom);
}

static void cursor_draw(void)
{
	if (!cursor_visible)
		return;

	uint32_t col = cell_col();
	uint32_t row = cell_row();
	lyrterm_cell_t cell = cell_get(col, row);

	uint32_t fg = cell.bg ? cell.bg : default_bg;
	uint32_t bg = cell.fg ? cell.fg : default_fg;
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
	uint32_t row = current_row();

	cursor_x = term_x0();

	if (row == scroll_bottom) {
		scroll_up_region(scroll_top, scroll_bottom);
		cursor_set_pos(scroll_bottom + 1, 1);
		return;
	}

	if (row + 1 >= rows) {
		scroll_up_region(scroll_top, scroll_bottom);
		cursor_set_pos(rows, 1);
		return;
	}

	cursor_y += _LYRTERM_LINE_HEIGHT;
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
		bold_attr = false;
		return;
	}

	for (int i = 0; i < nparams; i++) {
		int p = params[i];

		if (p == 0) {
			current_fg = default_fg;
			current_bg = default_bg;
			reverse_video = false;
			bold_attr = false;
		} else if (p == 1) {
			bold_attr = true;
		} else if (p == 2) {
			/* Dim ignored. */
		} else if (p == 3) {
			/* Italic ignored. */
		} else if (p == 4) {
			/* Underline ignored until cell attributes exist. */
		} else if (p == 5) {
			/* Blink ignored. */
		} else if (p == 7) {
			reverse_video = true;
		} else if (p == 8) {
			current_fg = current_bg;
		} else if (p == 22) {
			bold_attr = false;
		} else if (p == 23) {
			/* Italic off. */
		} else if (p == 24) {
			/* Underline off. */
		} else if (p == 25) {
			/* Blink off. */
		} else if (p == 27) {
			reverse_video = false;
		} else if (p == 28) {
			current_fg = default_fg;
		} else if (p == 39) {
			current_fg = default_fg;
		} else if (p == 49) {
			current_bg = default_bg;
		} else if (p >= 30 && p <= 37) {
			if (bold_attr)
				current_fg = pack_color(ansi_colors_bright[p - 30]);
			else
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
			} else if (mode == 5 && i + 1 < nparams) {
				int idx = params[++i];

				if (idx >= 0 && idx <= 7) {
					if (is_fg)
						current_fg = pack_color(ansi_colors[idx]);
					else
						current_bg = pack_color(ansi_colors[idx]);
				} else if (idx >= 8 && idx <= 15) {
					if (is_fg)
						current_fg = pack_color(ansi_colors_bright[idx - 8]);
					else
						current_bg = pack_color(ansi_colors_bright[idx - 8]);
				}
			}
		}
	}
}

static void ansi_handle_ed(int *params, int nparams)
{
	int mode = nparams ? params[0] : 0;

	uint32_t col = cell_col();
	uint32_t row = cell_row();

	if (mode == 2 || mode == 3) {
		clear_screen();
	} else if (mode == 1) {
		fill_rect(term_x0(), term_y0(), term_width(),
				  cursor_y - _LYRTERM_FONT_ASCENT - term_y0(), effective_bg());

		fill_rect(term_x0(), cursor_y - _LYRTERM_FONT_ASCENT,
				  cursor_x - term_x0() + _LYRTERM_LINE_WIDTH,
				  _LYRTERM_LINE_HEIGHT, effective_bg());

		cell_clear_range(0, 0, col, row);
	} else {
		uint32_t y = cursor_y - _LYRTERM_FONT_ASCENT;

		fill_rect(cursor_x, y, term_x0() + term_width() - cursor_x,
				  _LYRTERM_LINE_HEIGHT, effective_bg());

		if (y + _LYRTERM_LINE_HEIGHT < term_y0() + term_height()) {
			fill_rect(term_x0(), y + _LYRTERM_LINE_HEIGHT, term_width(),
					  term_y0() + term_height() - (y + _LYRTERM_LINE_HEIGHT),
					  effective_bg());
		}

		cell_clear_range(col, row, cols - 1, rows - 1);
	}
}

static void ansi_handle_el(int *params, int nparams)
{
	int mode = nparams ? params[0] : 0;

	uint32_t y = cursor_y - _LYRTERM_FONT_ASCENT;
	uint32_t col = cell_col();
	uint32_t row = cell_row();

	if (mode == 2) {
		fill_rect(term_x0(), y, term_width(), _LYRTERM_LINE_HEIGHT,
				  effective_bg());

		if (row < rows)
			cell_clear_range(0, row, cols - 1, row);
	} else if (mode == 1) {
		fill_rect(term_x0(), y, cursor_x - term_x0() + _LYRTERM_LINE_WIDTH,
				  _LYRTERM_LINE_HEIGHT, effective_bg());

		if (row < rows)
			cell_clear_range(0, row, col, row);
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

static void ansi_handle_cnl(int *params, int nparams)
{
	uint32_t n = (nparams && params[0]) ? (uint32_t)params[0] : 1;
	uint32_t row = current_row();

	row += n;
	if (row >= rows)
		row = rows - 1;

	cursor_set_pos(row + 1, 1);
}

static void ansi_handle_cpl(int *params, int nparams)
{
	uint32_t n = (nparams && params[0]) ? (uint32_t)params[0] : 1;
	uint32_t row = current_row();

	if (n > row)
		row = 0;
	else
		row -= n;

	cursor_set_pos(row + 1, 1);
}

static void ansi_handle_cha(int *params, int nparams)
{
	uint32_t col = (nparams && params[0]) ? (uint32_t)params[0] : 1;
	cursor_set_col(col);
}

static void ansi_handle_vpa(int *params, int nparams)
{
	uint32_t row = (nparams && params[0]) ? (uint32_t)params[0] : 1;
	cursor_set_row(row);
}

static void ansi_handle_cht(int *params, int nparams)
{
	uint32_t n = (nparams && params[0]) ? (uint32_t)params[0] : 1;

	while (n--) {
		uint32_t col = current_col();
		uint32_t next = ((col / 8) + 1) * 8;

		if (cols == 0)
			return;
		if (next >= cols)
			next = cols - 1;

		cursor_set_col(next + 1);
	}
}

static void ansi_handle_cbt(int *params, int nparams)
{
	uint32_t n = (nparams && params[0]) ? (uint32_t)params[0] : 1;

	while (n--) {
		uint32_t col = current_col();

		if (col == 0) {
			cursor_set_col(1);
			continue;
		}

		uint32_t prev = ((col - 1) / 8) * 8;
		cursor_set_col(prev + 1);
	}
}

static void ansi_handle_ich(int *params, int nparams)
{
	uint32_t n = (nparams && params[0]) ? (uint32_t)params[0] : 1;
	uint32_t row = current_row();
	uint32_t col = current_col();
	uint32_t max_cols = max_visible_cols();

	if (row >= max_visible_rows() || col >= max_cols)
		return;

	if (n > max_cols - col)
		n = max_cols - col;

	for (uint32_t c = max_cols - 1; c >= col + n && c < max_cols; c--)
		cell_buf[row][c] = cell_buf[row][c - n];

	for (uint32_t c = col; c < col + n; c++)
		cell_clear(c, row);

	redraw_line(row);
}

static void ansi_handle_dch(int *params, int nparams)
{
	uint32_t n = (nparams && params[0]) ? (uint32_t)params[0] : 1;
	uint32_t row = current_row();
	uint32_t col = current_col();
	uint32_t max_cols = max_visible_cols();

	if (row >= max_visible_rows() || col >= max_cols)
		return;

	if (n > max_cols - col)
		n = max_cols - col;

	for (uint32_t c = col; c + n < max_cols; c++)
		cell_buf[row][c] = cell_buf[row][c + n];

	for (uint32_t c = max_cols - n; c < max_cols; c++)
		cell_clear(c, row);

	redraw_line(row);
}

static void ansi_handle_ech(int *params, int nparams)
{
	uint32_t n = (nparams && params[0]) ? (uint32_t)params[0] : 1;
	uint32_t row = current_row();
	uint32_t col = current_col();
	uint32_t max_cols = max_visible_cols();

	if (row >= max_visible_rows() || col >= max_cols)
		return;

	if (n > max_cols - col)
		n = max_cols - col;

	for (uint32_t c = col; c < col + n; c++)
		cell_clear(c, row);

	redraw_line(row);
}

static void ansi_handle_il(int *params, int nparams)
{
	uint32_t n = (nparams && params[0]) ? (uint32_t)params[0] : 1;
	uint32_t row = current_row();

	if (row < scroll_top || row > scroll_bottom)
		return;

	while (n--)
		scroll_down_region(row, scroll_bottom);
}

static void ansi_handle_dl(int *params, int nparams)
{
	uint32_t n = (nparams && params[0]) ? (uint32_t)params[0] : 1;
	uint32_t row = current_row();

	if (row < scroll_top || row > scroll_bottom)
		return;

	while (n--)
		scroll_up_region(row, scroll_bottom);
}

static void ansi_handle_su(int *params, int nparams)
{
	uint32_t n = (nparams && params[0]) ? (uint32_t)params[0] : 1;

	while (n--)
		scroll_up_region(scroll_top, scroll_bottom);
}

static void ansi_handle_sd(int *params, int nparams)
{
	uint32_t n = (nparams && params[0]) ? (uint32_t)params[0] : 1;

	while (n--)
		scroll_down_region(scroll_top, scroll_bottom);
}

static void ansi_handle_decstbm(int *params, int nparams)
{
	uint32_t top = 1;
	uint32_t bottom = rows;

	if (rows == 0)
		return;

	if (nparams >= 1 && params[0])
		top = (uint32_t)params[0];
	if (nparams >= 2 && params[1])
		bottom = (uint32_t)params[1];

	if (top < 1)
		top = 1;
	if (bottom < 1)
		bottom = 1;
	if (top > rows)
		top = rows;
	if (bottom > rows)
		bottom = rows;

	if (top >= bottom) {
		scroll_top = 0;
		scroll_bottom = rows - 1;
	} else {
		scroll_top = top - 1;
		scroll_bottom = bottom - 1;
	}

	cursor_set_pos(1, 1);
}

static void ansi_handle_scp(int *params, int nparams)
{
	(void)params;
	(void)nparams;

	cursor_save();
}

static void ansi_handle_rcp(int *params, int nparams)
{
	(void)params;
	(void)nparams;

	cursor_restore();
}

static void ansi_handle_rep(int *params, int nparams)
{
	(void)params;
	(void)nparams;
}

static void ansi_handle_tbc(int *params, int nparams)
{
	(void)params;
	(void)nparams;
}

static void ansi_handle_dsr(int *params, int nparams)
{
	(void)params;
	(void)nparams;
}

static void ansi_handle_decset(int *params, int nparams)
{
	if (!csi_private_qmark)
		return;

	for (int i = 0; i < nparams; i++) {
		switch (params[i]) {
		case 1:
			break;
		case 7:
			break;
		case 12:
			break;
		case 25:
			cursor_visible = true;
			break;
		case 47:
		case 1047:
		case 1049:
			clear_screen();
			break;
		case 1000:
		case 1002:
		case 1003:
		case 1006:
		case 2004:
			break;
		default:
			break;
		}
	}
}

static void ansi_handle_decrst(int *params, int nparams)
{
	if (!csi_private_qmark)
		return;

	for (int i = 0; i < nparams; i++) {
		switch (params[i]) {
		case 1:
			break;
		case 7:
			break;
		case 12:
			break;
		case 25:
			cursor_visible = false;
			break;
		case 47:
		case 1047:
		case 1049:
			clear_screen();
			break;
		case 1000:
		case 1002:
		case 1003:
		case 1006:
		case 2004:
			break;
		default:
			break;
		}
	}
}

static const ansi_csi_handler_t ansi_csi_handlers[] = {
	{ '@', ansi_handle_ich },	  { 'A', ansi_handle_cuu },
	{ 'B', ansi_handle_cud },	  { 'C', ansi_handle_cuf },
	{ 'D', ansi_handle_cub },	  { 'E', ansi_handle_cnl },
	{ 'F', ansi_handle_cpl },	  { 'G', ansi_handle_cha },
	{ 'H', ansi_handle_cup },	  { 'I', ansi_handle_cht },
	{ 'J', ansi_handle_ed },	  { 'K', ansi_handle_el },
	{ 'L', ansi_handle_il },	  { 'M', ansi_handle_dl },
	{ 'P', ansi_handle_dch },	  { 'S', ansi_handle_su },
	{ 'T', ansi_handle_sd },	  { 'X', ansi_handle_ech },
	{ 'Z', ansi_handle_cbt },	  { '`', ansi_handle_cha },
	{ 'b', ansi_handle_rep },	  { 'd', ansi_handle_vpa },
	{ 'f', ansi_handle_cup },	  { 'g', ansi_handle_tbc },
	{ 'h', ansi_handle_decset },  { 'l', ansi_handle_decrst },
	{ 'm', ansi_handle_sgr },	  { 'n', ansi_handle_dsr },
	{ 'r', ansi_handle_decstbm }, { 's', ansi_handle_scp },
	{ 'u', ansi_handle_rcp },
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

static void ansi_dispatch_osc(const char *buf, size_t len)
{
	(void)buf;
	(void)len;
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

	scroll_top = 0;
	scroll_bottom = rows ? rows - 1 : 0;

	initialized = true;
	lyrterm_state_reset_internal(render_state);

	saved_cursor_x = cursor_x;
	saved_cursor_y = cursor_y;
	cursor_visible = true;
	bold_attr = false;
	csi_private = false;
	csi_private_qmark = false;
	esc_charset_skip = false;

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
	bold_attr = false;
}

void lyrterm_set_colors(uint32_t fg, uint32_t bg)
{
	default_fg = pack_color(fg);
	default_bg = pack_color(bg);
	current_fg = default_fg;
	current_bg = default_bg;
	reverse_video = false;
	bold_attr = false;
}

static void tab_advance(void)
{
	do {
		advance_cursor();
	} while ((((cursor_x - term_x0()) / _LYRTERM_LINE_WIDTH) % 8) != 0);
}

static void lyrterm_putcp_raw_locked(uint32_t codepoint)
{
	if (cursor_visible)
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

	if (cursor_visible)
		cursor_draw();
}

static void lyrterm_putch_locked(char raw)
{
	if (!initialized)
		return;

	uint8_t byte = (uint8_t)raw;

	if (esc_charset_skip) {
		esc_charset_skip = false;
		ansi_state = LYRTERM_ANSI_STATE_NORMAL;
		return;
	}

	if (ansi_state == LYRTERM_ANSI_STATE_OSC) {
		if (osc_saw_esc) {
			osc_saw_esc = false;
			if (raw == '\\') {
				osc_buf[osc_len] = '\0';
				ansi_dispatch_osc(osc_buf, osc_len);
				ansi_state = LYRTERM_ANSI_STATE_NORMAL;
			} else {
				ansi_state = LYRTERM_ANSI_STATE_NORMAL;
				lyrterm_putch_locked(raw);
			}
			return;
		}

		if (raw == '\007') {
			osc_buf[osc_len] = '\0';
			ansi_dispatch_osc(osc_buf, osc_len);
			ansi_state = LYRTERM_ANSI_STATE_NORMAL;
		} else if (raw == '\033') {
			osc_saw_esc = true;
		} else {
			if (osc_len + 1 < OSC_MAX)
				osc_buf[osc_len++] = raw;
		}
		return;
	}

	if (ansi_state == LYRTERM_ANSI_STATE_ESC) {
		if (raw == '[') {
			ansi_state = LYRTERM_ANSI_STATE_CSI;
			ansi_nparams = 0;
			csi_private = false;
			csi_private_qmark = false;
			memset(ansi_params, 0, sizeof(int) * ANSI_MAX_PARAMS);
		} else if (raw == ']') {
			ansi_state = LYRTERM_ANSI_STATE_OSC;
			osc_len = 0;
			osc_saw_esc = false;
			osc_buf[0] = '\0';
		} else if (raw == '7') {
			if (cursor_visible)
				cursor_erase();

			cursor_save();

			if (cursor_visible)
				cursor_draw();

			ansi_state = LYRTERM_ANSI_STATE_NORMAL;
		} else if (raw == '8') {
			if (cursor_visible)
				cursor_erase();

			cursor_restore();

			if (cursor_visible)
				cursor_draw();

			ansi_state = LYRTERM_ANSI_STATE_NORMAL;
		} else if (raw == 'c') {
			if (cursor_visible)
				cursor_erase();

			cursor_visible = true;
			current_fg = default_fg;
			current_bg = default_bg;
			reverse_video = false;
			bold_attr = false;
			scroll_top = 0;
			scroll_bottom = rows ? rows - 1 : 0;
			clear_screen();
			cursor_draw();

			ansi_state = LYRTERM_ANSI_STATE_NORMAL;
		} else if (raw == 'D') {
			if (cursor_visible)
				cursor_erase();

			if (current_row() == scroll_bottom)
				scroll_up_region(scroll_top, scroll_bottom);
			else if (current_row() + 1 < rows)
				cursor_y += _LYRTERM_LINE_HEIGHT;

			if (cursor_visible)
				cursor_draw();

			ansi_state = LYRTERM_ANSI_STATE_NORMAL;
		} else if (raw == 'E') {
			if (cursor_visible)
				cursor_erase();

			newline();

			if (cursor_visible)
				cursor_draw();

			ansi_state = LYRTERM_ANSI_STATE_NORMAL;
		} else if (raw == 'M') {
			if (cursor_visible)
				cursor_erase();

			if (current_row() == scroll_top)
				scroll_down_region(scroll_top, scroll_bottom);
			else if (current_row() > 0)
				cursor_y -= _LYRTERM_LINE_HEIGHT;

			if (cursor_visible)
				cursor_draw();

			ansi_state = LYRTERM_ANSI_STATE_NORMAL;
		} else if (raw == 'H') {
			ansi_state = LYRTERM_ANSI_STATE_NORMAL;
		} else if (raw == '=' || raw == '>') {
			ansi_state = LYRTERM_ANSI_STATE_NORMAL;
		} else if (raw == '(' || raw == ')' || raw == '*' || raw == '+') {
			esc_charset_skip = true;
			ansi_state = LYRTERM_ANSI_STATE_NORMAL;
		} else {
			ansi_state = LYRTERM_ANSI_STATE_NORMAL;
		}
		return;
	}

	if (ansi_state == LYRTERM_ANSI_STATE_CSI) {
		if (raw == '?' || raw == '>' || raw == '!') {
			csi_private = true;
			if (raw == '?')
				csi_private_qmark = true;
		} else if (raw >= '0' && raw <= '9') {
			ansi_params[ansi_nparams] =
				ansi_params[ansi_nparams] * 10 + (raw - '0');
		} else if (raw == ';' || raw == ':') {
			if (ansi_nparams < ANSI_MAX_PARAMS - 1)
				ansi_params[++ansi_nparams] = 0;
		} else {
			if (cursor_visible)
				cursor_erase();

			ansi_dispatch_csi(raw);

			ansi_nparams = 0;
			ansi_state = LYRTERM_ANSI_STATE_NORMAL;
			csi_private = false;
			csi_private_qmark = false;

			if (cursor_visible)
				cursor_draw();
		}

		return;
	}

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

void lyrterm_write(const char *buf, size_t len)
{
	if (!buf || len == 0)
		return;

	spinlock_acquire(&lyrterm_render_lock);

	for (size_t i = 0; i < len; i++)
		lyrterm_putch_locked(buf[i]);

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