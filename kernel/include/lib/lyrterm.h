#ifndef _LYR_LIB_LYRTERM_H
#define _LYR_LIB_LYRTERM_H

#include <stddef.h>
#include <stdint.h>
#include <lib/lyrterm_theme.h>
#include <limine.h>

/* for line cursor, uncomment this line */
// #define LYRTERM_LINE_CURSOR

#define LYRTERM_MAX_COLS 512
#define LYRTERM_MAX_ROWS 256

typedef enum {
	LYRTERM_ANSI_STATE_NORMAL,
	LYRTERM_ANSI_STATE_ESC,
	LYRTERM_ANSI_STATE_CSI,
	LYRTERM_ANSI_STATE_OSC,
} lyrterm_ansi_state_t;

typedef struct {
	uint32_t codepoint;
	uint32_t fg;
	uint32_t bg;
} lyrterm_cell_t;

typedef struct {
	uint32_t codepoint;
	uint8_t bytes_left;
} lyrterm_utf8_state_t;

typedef struct {
	uint32_t cursor_x;
	uint32_t cursor_y;
	uint32_t default_fg;
	uint32_t default_bg;
	uint32_t current_fg;
	uint32_t current_bg;
	int reverse_video;
	lyrterm_ansi_state_t ansi_state;
	int ansi_params[16];
	int ansi_nparams;
	char osc_buf[256];
	size_t osc_len;
	int osc_saw_esc;
	lyrterm_utf8_state_t utf8;
	lyrterm_cell_t cell_buf[LYRTERM_MAX_ROWS][LYRTERM_MAX_COLS];
} lyrterm_state_t;

typedef struct {
	uint64_t address;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t bpp;
	uint32_t size;
} lyrterm_framebuffer_info_t;

/* lyr kernel graphical terminal renderer */
void lyrterm_init(const struct limine_framebuffer *fb);
void lyrterm_putch(char c);
void lyrterm_putstr(const char *str);
void lyrterm_wbuf(const char *buf, size_t len);
size_t lyrterm_drain(size_t budget);
void lyrterm_flush(void);
size_t lyrterm_dropped_bytes(void);
void lyrterm_apply_theme(const lyrterm_theme_t *theme);
void lyrterm_get_size(uint32_t *cols_out, uint32_t *rows_out,
					  uint32_t *width_out, uint32_t *height_out);
int lyrterm_get_framebuffer_info(lyrterm_framebuffer_info_t *out);
int lyrterm_framebuffer_read(uint64_t off, void *buf, size_t len, size_t *done);
int lyrterm_framebuffer_write(uint64_t off, const void *buf, size_t len,
							  size_t *done);
void lyrterm_capture_state(lyrterm_state_t *out);
void lyrterm_restore_state(const lyrterm_state_t *in);
void lyrterm_update_state(lyrterm_state_t *state, const char *buf, size_t len);

#endif // _LYR_LIB_LYRTERM_H
