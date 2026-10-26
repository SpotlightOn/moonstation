#include "draw.h"

#include <stdio.h>
#include <string.h>

#include "display.h"
#include "font.h"

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void draw_pixel(int x, int y, uint16_t color, uint16_t *fb)
{
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    fb[y * SCREEN_W + x] = RGB565_SWAP(color);
}

void fill_rect(int x, int y, int w, int h, uint16_t color, uint16_t *fb)
{
    if (w <= 0 || h <= 0) return;
    int x0 = clampi(x, 0, SCREEN_W);
    int y0 = clampi(y, 0, SCREEN_H);
    int x1 = clampi(x + w, 0, SCREEN_W);
    int y1 = clampi(y + h, 0, SCREEN_H);
    uint16_t px = RGB565_SWAP(color);
    for (int row = y0; row < y1; row++) {
        uint16_t *line = fb + (size_t)row * SCREEN_W;
        for (int col = x0; col < x1; col++) line[col] = px;
    }
}

// Stamps a thickness x thickness block, which is all a line needs to look
// solid at the sizes used here.
static void stamp(int x, int y, int thickness, uint16_t px, uint16_t *fb)
{
    int t = thickness < 1 ? 1 : thickness;
    int o = t / 2;
    for (int row = y - o; row < y - o + t; row++) {
        if (row < 0 || row >= SCREEN_H) continue;
        uint16_t *line = fb + (size_t)row * SCREEN_W;
        for (int col = x - o; col < x - o + t; col++) {
            if (col < 0 || col >= SCREEN_W) continue;
            line[col] = px;
        }
    }
}

void draw_line(int x0, int y0, int x1, int y1, int thickness, uint16_t color,
               uint16_t *fb)
{
    uint16_t px = RGB565_SWAP(color);
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (1) {
        stamp(x0, y0, thickness, px, fb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void draw_disc(int cx, int cy, int r, uint16_t color, uint16_t *fb)
{
    if (r <= 0) return;
    uint16_t px = RGB565_SWAP(color);
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= SCREEN_H) continue;
        int dy = y - cy;
        // The disc is symmetric, so one half width is all a row needs.
        int half = 0;
        while (half * half + dy * dy <= r * r) half++;
        half--;
        if (half < 0) continue;
        int x0 = clampi(cx - half, 0, SCREEN_W);
        int x1 = clampi(cx + half + 1, 0, SCREEN_W);
        uint16_t *line = fb + (size_t)y * SCREEN_W;
        for (int x = x0; x < x1; x++) line[x] = px;
    }
}

void draw_ring(int cx, int cy, int r, int thickness, uint16_t color, uint16_t *fb)
{
    if (r <= 0 || thickness <= 0) return;
    int inner = r - thickness;
    uint16_t px = RGB565_SWAP(color);
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= SCREEN_H) continue;
        int dy = y - cy;
        uint16_t *line = fb + (size_t)y * SCREEN_W;
        for (int x = cx - r; x <= cx + r; x++) {
            if (x < 0 || x >= SCREEN_W) continue;
            int dx = x - cx;
            int d2 = dx * dx + dy * dy;
            if (d2 > r * r) continue;
            if (inner > 0 && d2 < inner * inner) continue;
            line[x] = px;
        }
    }
}

// One bit per pixel, as the clock font is packed.
static void draw_bits(const unsigned char *glyph, int gw, int gh, int x, int y,
                      uint16_t px, uint16_t *fb)
{
    int row_bytes = (gw + 7) / 8;
    for (int row = 0; row < gh; row++) {
        int py = y + row;
        if (py < 0 || py >= SCREEN_H) continue;
        uint16_t *line = fb + (size_t)py * SCREEN_W;
        for (int col = 0; col < gw; col++) {
            int px_x = x + col;
            if (px_x < 0 || px_x >= SCREEN_W) continue;
            if (glyph[row * row_bytes + col / 8] & (0x80 >> (col % 8))) {
                line[px_x] = px;
            }
        }
    }
}

// Antialiased pixel: the glyph carries alpha, the frame buffer the backdrop.
static void blend_pixel(int x, int y, uint16_t color, unsigned alpha, uint16_t *fb)
{
    if (alpha == 0) return;
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    uint16_t *dst = &fb[(size_t)y * SCREEN_W + x];
    if (alpha >= 255) {
        *dst = RGB565_SWAP(color);
        return;
    }
    uint16_t bg = RGB565_SWAP(*dst);
    unsigned inv = 255 - alpha;
    unsigned r = (((color >> 11) & 0x1F) * alpha + ((bg >> 11) & 0x1F) * inv) / 255;
    unsigned g = (((color >> 5) & 0x3F) * alpha + ((bg >> 5) & 0x3F) * inv) / 255;
    unsigned b = ((color & 0x1F) * alpha + (bg & 0x1F) * inv) / 255;
    *dst = RGB565_SWAP((uint16_t)((r << 11) | (g << 5) | b));
}

int draw_text_height(void)
{
    return font_text_H;
}

int draw_text_width(const char *s)
{
    int w = 0;
    for (const char *p = s; *p; p++) {
        int idx = font_text_index(*p);
        if (idx >= 0) w += font_text_glyphs[idx].adv;
    }
    return w;
}

void draw_text(const char *s, int x, int y, uint16_t color, uint16_t *fb)
{
    int pen = x;
    for (const char *p = s; *p; p++) {
        int idx = font_text_index(*p);
        if (idx < 0) continue;
        const font_glyph_t *g = &font_text_glyphs[idx];
        const unsigned char *bits = font_text_bits + g->off;
        for (int row = 0; row < g->h; row++) {
            int py = y + g->by + row;
            if (py < 0 || py >= SCREEN_H) continue;
            for (int col = 0; col < g->w; col++) {
                blend_pixel(pen + g->bx + col, py, color,
                            bits[row * g->w + col], fb);
            }
        }
        pen += g->adv;
    }
}

void draw_text_center(const char *s, int y, int dx, uint16_t color, uint16_t *fb)
{
    draw_text(s, (SCREEN_W - draw_text_width(s)) / 2 + dx, y, color, fb);
}

void draw_text_center_fit(const char *s, int y, int max_w, uint16_t color,
                          uint16_t *fb)
{
    char line[128];
    snprintf(line, sizeof(line), "%s", s);
    size_t n = strlen(line);
    if (draw_text_width(line) > max_w) {
        // Cut from the end and mark it with two dots, the same way the
        // weather lines do it.
        while (n > 2 && draw_text_width(line) > max_w) {
            line[--n] = '\0';
        }
        if (n >= 2) {
            line[n - 2] = '.';
            line[n - 1] = '.';
        }
    }
    draw_text_center(line, y, 0, color, fb);
}

int draw_time_height(void)
{
    return font_time_H;
}

int draw_time_width(const char *s)
{
    return (int)strlen(s) * font_time_W;
}

void draw_time_text(const char *s, int x, int y, uint16_t color, uint16_t *fb)
{
    uint16_t px = RGB565_SWAP(color);
    int pen = x;
    for (const char *p = s; *p; p++) {
        int idx = font_time_index(*p);
        if (idx >= 0) draw_bits(font_time_glyphs[idx], font_time_W, font_time_H,
                                pen, y, px, fb);
        pen += font_time_W;
    }
}
