#ifndef _LYR_DEV_CONSOLE_H
#define _LYR_DEV_CONSOLE_H

#include <stdint.h>

#define LYR_TCGETS 0x5401UL
#define LYR_TCSETS 0x5402UL
#define LYR_TCSETSW 0x5403UL
#define LYR_TCSETSF 0x5404UL
#define LYR_TIOCGWINSZ 0x5413UL
#define LYR_TIOCSWINSZ 0x5414UL

typedef struct lyr_winsize {
	uint16_t ws_row;
	uint16_t ws_col;
	uint16_t ws_xpixel;
	uint16_t ws_ypixel;
} lyr_winsize_t;

int console_init(void);
void console_input_put(uint8_t ch);

#endif /* _LYR_DEV_CONSOLE_H */
