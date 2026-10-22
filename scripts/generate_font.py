#!/usr/bin/env python3
"""Generate a small bitmap font header for the moonstation firmware."""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "./DejaVuSans-Bold.ttf"
OUT = os.path.join(os.path.dirname(__file__), "..", "src", "font.h")

def make_font(size, chars, cell_w=None, cell_h=None):
    font = ImageFont.truetype(FONT_PATH, size)
    if cell_h is None:
        cell_h = size + 4
    if cell_w is None:
        # Measure widest char, keep a small gap
        cell_w = max(font.getbbox(c)[2] - font.getbbox(c)[0] for c in chars) + 2
    glyphs = {}
    for ch in chars:
        img = Image.new("L", (cell_w, cell_h), 0)
        draw = ImageDraw.Draw(img)
        bbox = font.getbbox(ch)
        x = (cell_w - (bbox[2] - bbox[0])) // 2 - bbox[0]
        y = (cell_h - (bbox[3] - bbox[1])) // 2 - bbox[1]
        draw.text((x, y), ch, font=font, fill=255)
        # Pack bits row-major, MSB first
        bits = []
        for row in range(cell_h):
            byte = 0
            bit = 7
            for col in range(cell_w):
                if img.getpixel((col, row)) > 127:
                    byte |= (1 << bit)
                bit -= 1
                if bit < 0:
                    bits.append(byte)
                    byte = 0
                    bit = 7
            if bit != 7:
                bits.append(byte)
        glyphs[ch] = bytes(bits)
    return cell_w, cell_h, glyphs

def emit(name, size, chars):
    cw, ch, glyphs = make_font(size, chars)
    lines = []
    lines.append(f"#define {name}_W {cw}")
    lines.append(f"#define {name}_H {ch}")
    lines.append(f"static const unsigned char {name}_glyphs[{len(chars)}][{len(glyphs[chars[0]])}] = {{")
    for c in chars:
        b = glyphs[c]
        hexes = ", ".join(f"0x{byte:02X}" for byte in b)
        lines.append(f"    /* '{c}' */ {{ {hexes} }},")
    lines.append("};")
    # index mapping: simple ASCII for date font; time font only digits/colon
    lines.append(f"static int {name}_index(char c) {{")
    if chars == list("0123456789:"):
        lines.append("    if (c >= '0' && c <= '9') return c - '0';")
        lines.append("    if (c == ':') return 10;")
        lines.append("    return -1;")
    else:
        # printable ascii 32..126
        lines.append("    if (c < 32 || c > 126) return -1;")
        lines.append("    return c - 32;")
    lines.append("}")
    return "\n".join(lines) + "\n", chars

# Time font: large digits + colon
time_chars = list("0123456789:")
time_block, _ = emit("font_time", 48, time_chars)

# Date font: printable ASCII
date_chars = [chr(i) for i in range(32, 127)]
date_block, _ = emit("font_date", 16, date_chars)

# Need map date glyphs by char code offset 32; but emit uses chars list order.
# Rebuild date glyph array indexed by (c-32) by reordering chars
date_chars_sorted = [chr(i) for i in range(32, 127)]
# regenerate with sorted order (already sorted)
date_block2, _ = emit("font_date", 14, date_chars_sorted)

header = """#ifndef FONT_H
#define FONT_H

#include <stdint.h>

"""
header += time_block + "\n" + date_block2 + "\n"
header += """
static void font_draw_char(const unsigned char *glyph, int gw, int gh, int x, int y,
                           uint16_t color, uint16_t *fb) {
    for (int row = 0; row < gh; row++) {
        for (int col = 0; col < gw; col++) {
            int idx = row * ((gw + 7) / 8) + (col / 8);
            int bit = 7 - (col % 8);
            if (glyph[idx] & (1 << bit)) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < 240 && py >= 0 && py < 240) {
                    fb[py * 240 + px] = color;
                }
            }
        }
    }
}

#endif // FONT_H
"""

with open(OUT, "w") as f:
    f.write(header)
print("wrote", OUT)
