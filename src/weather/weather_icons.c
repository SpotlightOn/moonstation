#include "weather_icons.h"

#include "draw.h"

// Plain RGB565 values; draw.c does the byte swap for the panel.
#define ICON_AMBER 0xFD20
#define ICON_CLOUD 0xC618
#define ICON_RAIN 0x5DBF
#define ICON_SNOW 0xFFFF
#define ICON_FOG 0x8C71

// Three overlapping discs under a shared base: the classic cloud silhouette,
// cheap enough to render at any size.
static void cloud(int cx, int cy, int w, uint16_t color, uint16_t *fb)
{
    int lobe = w / 4;
    draw_disc(cx, cy - w / 8, w / 3, color, fb);
    draw_disc(cx - w / 4, cy + w / 20, lobe, color, fb);
    draw_disc(cx + w / 4, cy + w / 20, lobe, color, fb);
    fill_rect(cx - w / 2, cy + w / 20, w, w / 5, color, fb);
}

static void rain(int cx, int cy, int w, int drops, uint16_t color, uint16_t *fb)
{
    int thick = w / 13 < 1 ? 1 : w / 13;
    int len = w / 5;
    int left = cx - w / 3;
    int step = w / 3;
    for (int i = 0; i < drops; i++) {
        int x = left + i * step;
        draw_line(x, cy, x - len / 2, cy + len, thick, color, fb);
    }
}

static void sun(int cx, int cy, int size, uint16_t *fb)
{
    int r = size / 4;
    draw_disc(cx, cy, r, ICON_AMBER, fb);
    int thick = size / 13 < 1 ? 1 : size / 13;
    int inner = r + size / 12;
    int outer = r + size / 5;
    // Eight rays: the four axes and four diagonals.
    draw_line(cx - outer, cy, cx - inner, cy, thick, ICON_AMBER, fb);
    draw_line(cx + inner, cy, cx + outer, cy, thick, ICON_AMBER, fb);
    draw_line(cx, cy - outer, cx, cy - inner, thick, ICON_AMBER, fb);
    draw_line(cx, cy + inner, cx, cy + outer, thick, ICON_AMBER, fb);
    int a = outer * 7 / 10;
    int b = inner * 7 / 10;
    draw_line(cx - a, cy - a, cx - b, cy - b, thick, ICON_AMBER, fb);
    draw_line(cx + b, cy - b, cx + a, cy - a, thick, ICON_AMBER, fb);
    draw_line(cx - a, cy + a, cx - b, cy + b, thick, ICON_AMBER, fb);
    draw_line(cx + b, cy + b, cx + a, cy + a, thick, ICON_AMBER, fb);
}

// Crescent: everything inside the outer disc but outside a slightly smaller
// one shifted to the upper right.
static void moon(int cx, int cy, int size, uint16_t *fb)
{
    int r = size * 3 / 10;
    if (r < 3) r = 3;
    int ox = cx + r / 2;
    int oy = cy - r / 3;
    int ri = r * 4 / 5;
    for (int y = cy - r; y <= cy + r; y++) {
        for (int x = cx - r; x <= cx + r; x++) {
            int dx = x - cx;
            int dy = y - cy;
            if (dx * dx + dy * dy > r * r) continue;
            int ix = x - ox;
            int iy = y - oy;
            if (ix * ix + iy * iy < ri * ri) continue;
            draw_pixel(x, y, ICON_AMBER, fb);
        }
    }
}

static void bolt(int cx, int cy, int size, uint16_t *fb)
{
    int thick = size / 10 < 2 ? 2 : size / 10;
    int x0 = cx + size / 10;
    int x1 = cx - size / 12;
    int x2 = cx + size / 24;
    int x3 = cx - size / 10;
    draw_line(x0, cy, x1, cy + size / 4, thick, ICON_AMBER, fb);
    draw_line(x1, cy + size / 4, x2, cy + size / 4, thick, ICON_AMBER, fb);
    draw_line(x2, cy + size / 4, x3, cy + size / 2, thick, ICON_AMBER, fb);
}

static void flakes(int cx, int cy, int w, uint16_t *fb)
{
    int r = w / 14 < 1 ? 1 : w / 14;
    draw_disc(cx - w / 4, cy + w / 6, r, ICON_SNOW, fb);
    draw_disc(cx + w / 6, cy + w / 3, r, ICON_SNOW, fb);
    draw_disc(cx + w / 3, cy + w / 8, r, ICON_SNOW, fb);
}

weather_icon_kind_t weather_icon_kind(int cond)
{
    if (cond >= 200 && cond < 300) return WEATHER_ICON_STORM;
    if (cond >= 300 && cond < 400) return WEATHER_ICON_DRIZZLE;
    if (cond >= 500 && cond < 600) return WEATHER_ICON_RAIN;
    if (cond >= 600 && cond < 700) return WEATHER_ICON_SNOW;
    if (cond >= 700 && cond < 800) return WEATHER_ICON_FOG;
    if (cond == 800) return WEATHER_ICON_CLEAR;
    if (cond == 801 || cond == 802) return WEATHER_ICON_PARTLY;
    return WEATHER_ICON_CLOUD;
}

void weather_icon_draw(int cx, int cy, int size, weather_icon_kind_t kind,
                       bool night, uint16_t *fb)
{
    if (size < 8) size = 8;
    int upper = cy - size / 6;

    switch (kind) {
    case WEATHER_ICON_CLEAR:
        if (night) moon(cx, cy, size, fb);
        else sun(cx, cy, size, fb);
        break;

    case WEATHER_ICON_PARTLY:
        // Sun or moon behind the cloud, peeking out at the upper right.
        if (night) moon(cx + size / 5, cy - size / 4, size * 3 / 4, fb);
        else sun(cx + size / 5, cy - size / 4, size * 3 / 4, fb);
        cloud(cx - size / 10, upper + size / 8, size * 4 / 5, ICON_CLOUD, fb);
        break;

    case WEATHER_ICON_CLOUD:
        cloud(cx, cy, size, ICON_CLOUD, fb);
        break;

    case WEATHER_ICON_FOG:
        for (int i = 0; i < 3; i++) {
            int w = size - i * size / 5;
            int y = cy - size / 6 + i * size / 6;
            draw_line(cx - w / 2, y, cx + w / 2, y,
                      size / 13 < 1 ? 1 : size / 13, ICON_FOG, fb);
        }
        break;

    case WEATHER_ICON_DRIZZLE:
        cloud(cx, upper, size, ICON_CLOUD, fb);
        rain(cx, cy + size / 8, size, 2, ICON_RAIN, fb);
        break;

    case WEATHER_ICON_RAIN:
        cloud(cx, upper, size, ICON_CLOUD, fb);
        rain(cx, cy + size / 12, size, 3, ICON_RAIN, fb);
        break;

    case WEATHER_ICON_SNOW:
        cloud(cx, upper, size, ICON_CLOUD, fb);
        flakes(cx, cy + size / 8, size, fb);
        break;

    case WEATHER_ICON_STORM:
        cloud(cx, upper - size / 10, size, ICON_CLOUD, fb);
        bolt(cx, cy, size, fb);
        rain(cx, cy + size / 10, size, 2, ICON_RAIN, fb);
        break;
    }
}
