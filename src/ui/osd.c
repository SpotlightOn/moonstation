#include "osd.h"
#include "display.h"
#include "draw.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "osd";

// The overlay sits in the middle of the round panel: 64 px high, 152 px wide,
// so every row of the box stays inside the circle (at the top edge of the box
// the visible width is 231 px, at the bottom 206 px).
#define OSD_X 44
#define OSD_Y 88
#define OSD_WIDTH 152
#define OSD_HEIGHT 64
#define OSD_DURATION_US 1500000

#define OSD_BG 0x0841     // a touch above the panel's black, so the box reads
#define OSD_FRAME 0x4208  // border, dimmer than the track
#define OSD_TEXT 0xFFFF
#define OSD_DIM 0x8C71
#define OSD_TRACK 0x2945
#define OSD_FILL 0xFD20   // amber, the same accent the weather page uses

static uint16_t *saved = NULL;  // the pixels the overlay covers
static uint16_t *rect = NULL;   // the pixels the overlay pushed
static bool visible = false;
static bool warned = false;
static int64_t hide_at_us = 0;
static uint32_t shown_sum = 0;  // checksum of the overlay as it was drawn

static bool has_buffers(void)
{
    if (saved && rect) return true;
    // Two rectangles of 19 kB; PSRAM is the only place with room to spare.
    const size_t bytes = OSD_WIDTH * OSD_HEIGHT * sizeof(uint16_t);
    if (!saved) {
        saved = heap_caps_aligned_alloc(64, bytes,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!rect) {
        rect = heap_caps_aligned_alloc(64, bytes,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!saved || !rect) {
        if (saved) heap_caps_free(saved);
        if (rect) heap_caps_free(rect);
        saved = NULL;
        rect = NULL;
        if (!warned) {
            ESP_LOGW(TAG, "no PSRAM for the overlay");
            warned = true;
        }
        return false;
    }
    return true;
}

// Copies the overlay rectangle out of the frame buffer, row by row: the frame
// buffer is 240 px wide, the rectangle only 152.
static void grab(uint16_t *dst, const uint16_t *fb)
{
    for (int y = 0; y < OSD_HEIGHT; y++) {
        memcpy(dst + y * OSD_WIDTH, fb + (OSD_Y + y) * SCREEN_W + OSD_X, OSD_WIDTH * 2);
    }
}

static void put(uint16_t *fb, const uint16_t *src)
{
    for (int y = 0; y < OSD_HEIGHT; y++) {
        memcpy(fb + (OSD_Y + y) * SCREEN_W + OSD_X, src + y * OSD_WIDTH, OSD_WIDTH * 2);
    }
}

// FNV-1a over the rectangle as it currently is in the frame buffer. Comparing
// it against the checksum taken right after the overlay was drawn tells us
// whether a mode has painted over the overlay since - without every mode
// having to report its pushes.
static uint32_t region_sum(const uint16_t *fb)
{
    uint32_t h = 2166136261u;
    for (int y = 0; y < OSD_HEIGHT; y++) {
        const uint8_t *row = (const uint8_t *)(fb + (OSD_Y + y) * SCREEN_W + OSD_X);
        for (int i = 0; i < OSD_WIDTH * 2; i++) {
            h = (h ^ row[i]) * 16777619u;
        }
    }
    return h;
}

static void draw_overlay(int percent, uint16_t *fb)
{
    fill_rect(OSD_X, OSD_Y, OSD_WIDTH, OSD_HEIGHT, OSD_BG, fb);
    fill_rect(OSD_X, OSD_Y, OSD_WIDTH, 1, OSD_FRAME, fb);
    fill_rect(OSD_X, OSD_Y + OSD_HEIGHT - 1, OSD_WIDTH, 1, OSD_FRAME, fb);
    fill_rect(OSD_X, OSD_Y, 1, OSD_HEIGHT, OSD_FRAME, fb);
    fill_rect(OSD_X + OSD_WIDTH - 1, OSD_Y, 1, OSD_HEIGHT, OSD_FRAME, fb);

    draw_text_center("Volume", OSD_Y + 3, 0, OSD_DIM, fb);

    char value[8];
    snprintf(value, sizeof(value), "%d%%", percent);
    draw_text_center(value, OSD_Y + 23, 0, OSD_TEXT, fb);

    const int bar_x = OSD_X + 14;
    const int bar_w = OSD_WIDTH - 28;
    const int bar_y = OSD_Y + 46;
    const int bar_h = 8;
    fill_rect(bar_x, bar_y, bar_w, bar_h, OSD_TRACK, fb);
    const int fill = bar_w * percent / 100;
    if (fill > 0) fill_rect(bar_x, bar_y, fill, bar_h, OSD_FILL, fb);
}

void osd_init(void)
{
    visible = false;
    hide_at_us = 0;
    shown_sum = 0;
}

void osd_show_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    uint16_t *fb = display_get_framebuffer();
    if (!fb || !has_buffers()) return;

    // Keep a fresh copy of what is underneath only when the overlay is gone or
    // something painted over it since the last show; otherwise the copy from
    // the first show still holds the mode's pixels, not the old bar.
    if (!visible || region_sum(fb) != shown_sum) grab(saved, fb);

    draw_overlay(percent, fb);

    grab(rect, fb);
    esp_lcd_panel_draw_bitmap(display_get_panel(), OSD_X, OSD_Y,
                              OSD_X + OSD_WIDTH, OSD_Y + OSD_HEIGHT, rect);

    shown_sum = region_sum(fb);
    visible = true;
    hide_at_us = esp_timer_get_time() + OSD_DURATION_US;
}

void osd_on_wake(int64_t now_us)
{
    if (!visible || now_us < hide_at_us) return;
    visible = false;

    uint16_t *fb = display_get_framebuffer();
    if (!fb || !saved) return;

    // A mode that redrew the area has already replaced the overlay with
    // something newer; putting the old pixels back would erase it.
    if (region_sum(fb) != shown_sum) return;

    put(fb, saved);
    esp_lcd_panel_draw_bitmap(display_get_panel(), OSD_X, OSD_Y,
                              OSD_X + OSD_WIDTH, OSD_Y + OSD_HEIGHT, saved);
}

int64_t osd_next_wake_us(int64_t now_us)
{
    if (!visible) return INT64_MAX;
    const int64_t left = hide_at_us - now_us;
    return left > 0 ? left : 0;
}
