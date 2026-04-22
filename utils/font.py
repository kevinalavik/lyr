#!/usr/bin/env python3

import argparse
import os
from PIL import Image, ImageFont, ImageDraw

THRESHOLD = 128


def get_all_codepoints(font_path):
    try:
        from fonttools import ttLib

        tt = ttLib.TTFont(font_path, lazy=True)

        real_glyphs = set()

        if "glyf" in tt:
            glyf_table = tt["glyf"]
            for name in tt.getGlyphOrder():
                try:
                    g = glyf_table[name]
                    if g is not None and g.numberOfContours != 0:
                        real_glyphs.add(name)
                except Exception:
                    pass
        elif "CFF " in tt or "CFF2" in tt:
            tag = "CFF " if "CFF " in tt else "CFF2"
            top_dict = tt[tag].cff.topDictIndex[0]
            cs = top_dict.CharStrings
            for name, charstring in cs.items():
                try:
                    bounds = charstring.calcBounds(cs)
                    if bounds is not None:
                        real_glyphs.add(name)
                except Exception:
                    real_glyphs.add(name)
        else:
            for name, (advance, _lsb) in tt["hmtx"].metrics.items():
                if advance > 0:
                    real_glyphs.add(name)

        if "hmtx" in tt:
            for name, (advance, _lsb) in tt["hmtx"].metrics.items():
                if advance > 0 and name != ".notdef":
                    real_glyphs.add(name)

        cmap = tt.getBestCmap()
        if cmap is None:
            raise ValueError("no Unicode cmap table")

        codepoints = sorted(
            cp for cp, glyph_name in cmap.items()
            if cp > 0x1F
            and not (0xD800 <= cp <= 0xDFFF)
            and glyph_name != ".notdef"
            and glyph_name in real_glyphs
        )

        tt.close()
        return codepoints

    except ImportError:
        pass

    print("[warn] fonttools not found, falling back to brute-force probe up to U+FFFF")
    font = ImageFont.truetype(font_path, 16)
    codepoints = []
    for cp in range(0x20, 0x10000):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        try:
            bbox = font.getbbox(chr(cp))
            if bbox[2] - bbox[0] > 0:
                codepoints.append(cp)
        except Exception:
            pass
    return codepoints


def image_to_bitmap(img):
    pixels = img.load()
    width, height = img.size
    data = []
    for y in range(height):
        byte = 0
        bit_count = 0
        for x in range(width):
            bit = 1 if pixels[x, y] > THRESHOLD else 0
            byte = (byte << 1) | bit
            bit_count += 1
            if bit_count == 8:
                data.append(byte)
                byte = 0
                bit_count = 0
        if bit_count > 0:
            byte <<= (8 - bit_count)
            data.append(byte)
    return data


def generate(font_path, output_path, font_size, codepoints, cell_width=None):
    base   = os.path.splitext(output_path)[0]
    h_path = base + ".h"
    c_path = base + ".c"
    h_name = os.path.basename(h_path)
    guard  = h_name.upper().replace(".", "_").replace("-", "_")

    font = ImageFont.truetype(font_path, font_size)
    ascent, descent = font.getmetrics()
    max_height = ascent + descent

    ascii_cps = [cp for cp in range(0x20, 0x7F) if cp in set(codepoints)]
    if not ascii_cps:
        ascii_cps = codepoints

    max_width = 0
    for cp in ascii_cps:
        try:
            bbox = font.getbbox(chr(cp))
        except Exception:
            continue
        w = bbox[2] - bbox[0]
        if w > 0:
            max_width = max(max_width, w)

    if max_width == 0:
        raise ValueError("no renderable glyphs found")

    if cell_width is not None:
        max_width = cell_width

    bytes_per_row = (max_width + 7) // 8
    max_bytes     = bytes_per_row * max_height
    glyph_count   = len(codepoints)

    print(f"cell={max_width}x{max_height}  bytes_per_glyph={max_bytes}")

    from collections import Counter
    all_bitmaps = []

    for cp in codepoints:
        image = Image.new("L", (max_width, max_height), 0)
        draw  = ImageDraw.Draw(image)

        try:
            bbox = font.getbbox(chr(cp))
            x_off = -bbox[0]
            y_off = ascent - bbox[3]
            draw.text((x_off, y_off), chr(cp), fill=255, font=font)
        except Exception:
            pass

        bitmap = tuple((image_to_bitmap(image) + [0] * max_bytes)[:max_bytes])
        all_bitmaps.append((cp, bitmap))

    freq     = Counter(bmp for _, bmp in all_bitmaps)
    sentinel = freq.most_common(1)[0][0]
    dropped  = freq[sentinel]
    sentinel_hex = ", ".join(f"0x{b:02X}" for b in sentinel)

    glyph_bitmaps = [(cp, bmp) for cp, bmp in all_bitmaps if bmp != sentinel]
    glyph_count   = len(glyph_bitmaps)

    print(f"total={len(all_bitmaps)} dropped={dropped} kept={glyph_count}")

    with open(h_path, "w", encoding="utf-8") as f:
        w = f.write
        w(f"#ifndef {guard}\n#define {guard}\n\n#include <stdint.h>\n\n")
        w(f"#define _LYRTERM_FONT_GLYPH_COUNT {glyph_count}\n")
        w(f"#define _LYRTERM_FONT_WIDTH {max_width}\n")
        w(f"#define _LYRTERM_FONT_HEIGHT {max_height}\n")
        w(f"#define _LYRTERM_FONT_ASCENT {ascent}\n")
        w(f"#define _LYRTERM_FONT_DESCENT {descent}\n")
        w(f"#define _LYRTERM_FONT_MAX_BYTES {max_bytes}\n\n")
        w(f"#define _LYRTERM_FONT_SENTINEL_BYTES {{ {sentinel_hex} }}\n\n")

        w("typedef struct { uint32_t codepoint; uint16_t idx; } _lyrterm_glyph_entry_t;\n\n")
        w(f"extern const _lyrterm_glyph_entry_t _lyrterm_glyph_map[{glyph_count}];\n")
        w(f"extern const uint8_t _lyrterm_font[{glyph_count}][{max_bytes}];\n")
        w(f"extern const uint8_t _lyrterm_font_sentinel[{max_bytes}];\n\n")

        w("static inline int _lyrterm_find_glyph(uint32_t codepoint) {\n")
        w("    int lo = 0, hi = _LYRTERM_FONT_GLYPH_COUNT - 1;\n")
        w("    while (lo <= hi) {\n")
        w("        int mid = (lo + hi) / 2;\n")
        w("        uint32_t cp = _lyrterm_glyph_map[mid].codepoint;\n")
        w("        if (cp == codepoint) return _lyrterm_glyph_map[mid].idx;\n")
        w("        if (cp < codepoint) lo = mid + 1; else hi = mid - 1;\n")
        w("    }\n")
        w("    return -1;\n}\n\n#endif\n")

    with open(c_path, "w", encoding="utf-8") as f:
        w = f.write
        w(f'#include "{h_name}"\n\n')
        w(f"const uint8_t _lyrterm_font_sentinel[{max_bytes}] = {{ {sentinel_hex} }};\n\n")

        w(f"const _lyrterm_glyph_entry_t _lyrterm_glyph_map[{glyph_count}] = {{\n")
        for i, (cp, _) in enumerate(glyph_bitmaps):
            w(f"    {{0x{cp:06X}, {i}}},\n")
        w("};\n\n")

        w(f"const uint8_t _lyrterm_font[{glyph_count}][{max_bytes}] = {{\n")
        for _, bitmap in glyph_bitmaps:
            w("    {" + ", ".join(f"0x{b:02X}" for b in bitmap) + "},\n")
        w("};\n")

    print(f"wrote {h_path} and {c_path}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("font")
    parser.add_argument("output")
    parser.add_argument("--size", type=int, default=16)
    parser.add_argument("--cell-width", type=int, default=None)
    parser.add_argument("--first", type=int, default=None)
    parser.add_argument("--last", type=int, default=None)
    args = parser.parse_args()

    if args.first is not None and args.last is not None:
        codepoints = [
            cp for cp in range(args.first, args.last + 1)
            if not (0xD800 <= cp <= 0xDFFF)
        ]
    else:
        codepoints = get_all_codepoints(args.font)

    generate(args.font, args.output, args.size, codepoints, args.cell_width)


if __name__ == "__main__":
    main()