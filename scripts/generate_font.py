#!/usr/bin/env python3
"""Generate the bitmap font header for the moonstation firmware.

Two fonts come out of this script, both into src/ui/font.h:

  font_time  the big clock digits, fixed 35x52 cells, one bit per pixel.
             DejaVu Sans Bold 48, the look the clock had from the start.
  font_text  body text, proportional, eight bit alpha for antialiasing.
             Roboto Medium 16, the system font family of Android.

The TTFs live in scripts/fonts. Run this script from the project root or from
scripts/, it writes the header next to the other sources.
"""
import os

from PIL import Image, ImageDraw, ImageFont

FONT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fonts")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "ui", "font.h")

TIME_TTF = os.path.join(FONT_DIR, "DejaVuSans-Bold.ttf")
TEXT_TTF = os.path.join(FONT_DIR, "Roboto-Medium.ttf")

# C string form of the degree sign. The firmware is ASCII only, the glyph
# table carries 0xB0 as a single byte.
DEGREE = 0xB0


def pack_1bpp(img):
    """Row major, most significant bit first, as the panel writer expects."""
    w, h = img.size
    bits = bytearray()
    for row in range(h):
        byte = 0
        bit = 7
        for col in range(w):
            if img.getpixel((col, row)) > 127:
                byte |= 1 << bit
            bit -= 1
            if bit < 0:
                bits.append(byte)
                byte = 0
                bit = 7
        if bit != 7:
            bits.append(byte)
    return bytes(bits)


def make_time_font(path, size, chars):
    """Fixed cell font: every glyph sits centered in the same box.

    Unchanged from the first version of this script - the clock on the panel
    was never the problem, so its metrics stay byte for byte the same.
    """
    font = ImageFont.truetype(path, size)
    cell_w = max(font.getbbox(c)[2] - font.getbbox(c)[0] for c in chars) + 2
    cell_h = size + 4
    glyphs = {}
    for ch in chars:
        img = Image.new("L", (cell_w, cell_h), 0)
        draw = ImageDraw.Draw(img)
        bbox = font.getbbox(ch)
        x = (cell_w - (bbox[2] - bbox[0])) // 2 - bbox[0]
        y = (cell_h - (bbox[3] - bbox[1])) // 2 - bbox[1]
        draw.text((x, y), ch, font=font, fill=255)
        glyphs[ch] = pack_1bpp(img)
    return cell_w, cell_h, glyphs


def make_text_font(path, size, chars):
    """Proportional font with a shared baseline and eight bit alpha.

    Each glyph is cropped to its ink box and keeps the offset from the pen
    position, so a line is laid out like a normal text renderer would: the
    advance comes from the font, not from the widest glyph in the set.
    """
    font = ImageFont.truetype(path, size)
    ascent, descent = font.getmetrics()
    line_h = ascent + descent
    blob = bytearray()
    glyphs = []
    for ch in chars:
        adv = max(1, int(round(font.getlength(ch))))
        x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
        if x1 <= x0 or y1 <= y0:  # space and other blank glyphs
            glyphs.append((0, 0, 0, 0, adv, len(blob)))
            continue
        w = max(adv, x1) - min(0, x0)
        h = y1 - y0
        bx = x0
        by = ascent + y0
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((-bx, ascent), ch, font=font, fill=255, anchor="ls")
        glyphs.append((w, h, bx, by, adv, len(blob)))
        blob += img.tobytes()
    return line_h, bytes(blob), glyphs


def emit_time(chars, cell_w, cell_h, glyphs):
    row_bytes = (cell_w + 7) // 8
    lines = [
        f"#define font_time_W {cell_w}",
        f"#define font_time_H {cell_h}",
        f"static const unsigned char font_time_glyphs[{len(chars)}][{row_bytes * cell_h}] = {{",
    ]
    for ch in chars:
        hexes = ", ".join(f"0x{b:02X}" for b in glyphs[ch])
        lines.append(f"    /* '{ch}' */ {{ {hexes} }},")
    lines.append("};")
    lines.append("static int font_time_index(char c) {")
    lines.append("    if (c >= '0' && c <= '9') return c - '0';")
    lines.append("    if (c == ':') return 10;")
    lines.append("    if (c == '-') return 11;")
    lines.append("    return -1;")
    lines.append("}")
    return "\n".join(lines) + "\n"


def emit_text(line_h, blob, glyphs, chars):
    lines = [
        f"#define font_text_H {line_h}",
        "typedef struct {",
        "    uint8_t w, h;      // ink box",
        "    int8_t bx, by;     // where the ink box starts, relative to the pen",
        "    uint8_t adv;       // pen advance",
        "    uint16_t off;      // start in font_text_bits",
        "} font_glyph_t;",
        "",
        f"static const unsigned char font_text_bits[{len(blob)}] = {{",
    ]
    for i in range(0, len(blob), 16):
        chunk = blob[i:i + 16]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    lines.append("};")
    lines.append("")
    lines.append(f"static const font_glyph_t font_text_glyphs[{len(chars)}] = {{")
    for ch, (w, h, bx, by, adv, off) in zip(chars, glyphs):
        label = ch if ch != "\x00" else "NUL"
        lines.append(
            f"    /* 0x{ord(ch):02X} {label!r} */ {{ {w}, {h}, {bx}, {by}, {adv}, {off} }},"
        )
    lines.append("};")
    lines.append("static int font_text_index(char c) {")
    lines.append("    unsigned char u = (unsigned char)c;")
    lines.append("    if (u >= 32 && u <= 126) return u - 32;")
    lines.append(f"    if (u == 0x{DEGREE:02X}) return {len(chars) - 1};")
    lines.append("    return -1;")
    lines.append("}")
    return "\n".join(lines) + "\n"


time_chars = list("0123456789:-")
time_cw, time_ch, time_glyphs = make_time_font(TIME_TTF, 48, time_chars)

text_chars = [chr(i) for i in range(32, 127)] + [chr(DEGREE)]
text_h, text_blob, text_glyphs = make_text_font(TEXT_TTF, 16, text_chars)

header = """#ifndef FONT_H
#define FONT_H

#include <stdint.h>

// Generated by scripts/generate_font.py - do not edit.
// font_time: DejaVu Sans Bold 48, fixed cells, one bit per pixel.
// font_text: Roboto Medium 16, proportional, eight bit alpha.

"""
header += emit_time(time_chars, time_cw, time_ch, time_glyphs)
header += "\n" + emit_text(text_h, text_blob, text_glyphs, text_chars)
header += "\n#endif // FONT_H\n"

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w") as f:
    f.write(header)

print("wrote", os.path.normpath(OUT))
print("font_time", time_cw, "x", time_ch, "cell")
print("font_text line height", text_h, "alpha bytes", len(text_blob))
