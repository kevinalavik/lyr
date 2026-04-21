#!/usr/bin/env python3

import argparse
from PIL import Image, ImageFont, ImageDraw

THRESHOLD = 128


def render_glyph(char, font):
    bbox = font.getbbox(char)
    width = bbox[2] - bbox[0]
    height = bbox[3] - bbox[1]

    if width == 0 or height == 0:
        width, height = 1, 1

    image = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(image)
    draw.text((-bbox[0], -bbox[1]), char, fill=255, font=font)

    return image


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

def generate(font_path, output_path, font_size, first_char, last_char):
    font = ImageFont.truetype(font_path, font_size)

    glyph_bitmaps = []
    max_width = 0
    max_height = 0

    # Pass 1: determine max glyph size
    for c in range(first_char, last_char + 1):
        char = chr(c)
        bbox = font.getbbox(char)

        width = bbox[2] - bbox[0]
        height = bbox[3] - bbox[1]

        if width == 0: width = 1
        if height == 0: height = 1

        max_width = max(max_width, width)
        max_height = max(max_height, height)

    bytes_per_row = (max_width + 7) // 8
    max_bytes = bytes_per_row * max_height
    glyph_count = last_char - first_char + 1

    # Pass 2: render into fixed-size buffer
    for c in range(first_char, last_char + 1):
        char = chr(c)

        bbox = font.getbbox(char)
        width = bbox[2] - bbox[0]
        height = bbox[3] - bbox[1]

        if width == 0: width = 1
        if height == 0: height = 1

        image = Image.new("L", (max_width, max_height), 0)
        draw = ImageDraw.Draw(image)

        # top-left aligned (simple, predictable)
        draw.text((-bbox[0], -bbox[1]), char, fill=255, font=font)

        bitmap = image_to_bitmap(image)
        glyph_bitmaps.append((c, bitmap))

    # Write output
    with open(output_path, "w") as f:
        f.write("#ifndef FONT_H\n#define FONT_H\n\n")
        f.write("#include <stdint.h>\n\n")

        f.write(f"#define _LYRTERM_FONT_FIRST_CHAR {first_char}\n")
        f.write(f"#define _LYRTERM_FONT_LAST_CHAR {last_char}\n")
        f.write(f"#define _LYRTERM_FONT_GLYPH_COUNT {glyph_count}\n")
        f.write(f"#define _LYRTERM_FONT_WIDTH {max_width}\n")
        f.write(f"#define _LYRTERM_FONT_HEIGHT {max_height}\n")
        f.write(f"#define _LYRTERM_FONT_MAX_BYTES {max_bytes}\n\n")

        f.write(f"const uint8_t _lyrterm_font[{glyph_count}][{max_bytes}] = {{\n")

        for i, (code, bitmap) in enumerate(glyph_bitmaps):
            f.write(f"    // '{chr(code)}' ({code})\n")
            f.write("    {")

            for j, b in enumerate(bitmap):
                f.write(f"0x{b:02X}")
                if j != max_bytes - 1:
                    f.write(", ")

            f.write("}")
            if i != glyph_count - 1:
                f.write(",")
            f.write("\n")

        f.write("};\n\n#endif\n")

def main():
    parser = argparse.ArgumentParser(
        description="Generate C bitmap font header from a TTF/OTF font"
    )

    parser.add_argument("font", help="Path to .ttf/.otf font file")
    parser.add_argument("output", help="Output .h file")

    parser.add_argument("--size", type=int, default=16)
    parser.add_argument("--first", type=int, default=32)
    parser.add_argument("--last", type=int, default=126)

    args = parser.parse_args()

    generate(
        font_path=args.font,
        output_path=args.output,
        font_size=args.size,
        first_char=args.first,
        last_char=args.last,
    )


if __name__ == "__main__":
    main()