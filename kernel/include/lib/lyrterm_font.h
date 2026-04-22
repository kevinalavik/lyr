#ifndef LYRTERM_FONT_H
#define LYRTERM_FONT_H

#include <stdint.h>

#define _LYRTERM_FONT_GLYPH_COUNT 276
#define _LYRTERM_FONT_WIDTH 8
#define _LYRTERM_FONT_HEIGHT 16
#define _LYRTERM_FONT_MAX_BYTES 16

/* Sentinel bitmap emitted for any codepoint not in the map.
 * 63119 of 63395 glyphs were identical to this and omitted. */
#define _LYRTERM_FONT_SENTINEL_BYTES                                      \
	{                                                                     \
		0x00, 0x00, 0x00, 0x78, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, \
			0x08, 0x08, 0x78, 0x00, 0x00                                  \
	}

typedef struct {
	uint32_t codepoint;
	uint16_t idx;
} _lyrterm_glyph_entry_t;

extern const _lyrterm_glyph_entry_t _lyrterm_glyph_map[276];
extern const uint8_t _lyrterm_font[276][16];
extern const uint8_t _lyrterm_font_sentinel[16];

static inline int _lyrterm_find_glyph(uint32_t codepoint)
{
	int lo = 0, hi = _LYRTERM_FONT_GLYPH_COUNT - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		uint32_t cp = _lyrterm_glyph_map[mid].codepoint;
		if (cp == codepoint)
			return (int)_lyrterm_glyph_map[mid].idx;
		if (cp < codepoint)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

#endif
