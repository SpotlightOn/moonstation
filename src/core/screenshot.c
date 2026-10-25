#include "screenshot.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "features.h"

#include "display.h"
#include "draw.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdcard.h"

#if SCREENSHOT_ENABLE

static const char *TAG = "shot";

#define SHOT_DIR "/sdcard/screens"
#define BMP_HEADER 54
// 24 bit rows of 240 px are 720 bytes, already a multiple of four, so no
// padding is needed and the file layout is just header plus screen rows.
#define BMP_ROW_BYTES (SCREEN_W * 3)

// ST7789-style BMP header, little endian, bottom-up rows. Written by hand so
// the debug build needs no image library.
static void make_header(uint8_t *h, uint32_t size)
{
    memset(h, 0, BMP_HEADER);
    h[0] = 'B';
    h[1] = 'M';
    h[2] = (uint8_t)(size);
    h[3] = (uint8_t)(size >> 8);
    h[4] = (uint8_t)(size >> 16);
    h[5] = (uint8_t)(size >> 24);
    h[10] = BMP_HEADER; // offset of the pixel data
    h[14] = 40;         // DIB header size
    h[18] = (uint8_t)(SCREEN_W);
    h[19] = (uint8_t)(SCREEN_W >> 8);
    h[20] = (uint8_t)(SCREEN_W >> 16);
    h[21] = (uint8_t)(SCREEN_W >> 24);
    h[22] = (uint8_t)(SCREEN_H);
    h[23] = (uint8_t)(SCREEN_H >> 8);
    h[24] = (uint8_t)(SCREEN_H >> 16);
    h[25] = (uint8_t)(SCREEN_H >> 24);
    h[26] = 1;  // planes
    h[28] = 24; // bits per pixel
    uint32_t data = (uint32_t)BMP_ROW_BYTES * SCREEN_H;
    h[34] = (uint8_t)(data);
    h[35] = (uint8_t)(data >> 8);
    h[36] = (uint8_t)(data >> 16);
    h[37] = (uint8_t)(data >> 24);
}

// The frame buffer holds RGB565 with swapped bytes for the panel (see
// RGB565_SWAP in display.h); the BMP wants one byte per channel in BGR order.
static void row_to_bmp(const uint16_t *src, uint8_t *dst)
{
    for (int x = 0; x < SCREEN_W; x++) {
        uint16_t px = RGB565_SWAP(src[x]);
        unsigned r = (px >> 11) & 0x1F;
        unsigned g = (px >> 5) & 0x3F;
        unsigned b = px & 0x1F;
        *dst++ = (uint8_t)((b * 255 + 15) / 31);
        *dst++ = (uint8_t)((g * 255 + 31) / 63);
        *dst++ = (uint8_t)((r * 255 + 15) / 31);
    }
}

// Shorten the caller's mode name to something FAT likes in 8.3.
static void shot_name(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < cap && o < 8; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        out[o++] = ok ? c : '_';
    }
    out[o] = '\0';
    if (o == 0) snprintf(out, cap, "frame");
}

#if SCREENSHOT_SERIAL
// One line per 48 px: "SHOT <name> <first pixel> <4 hex digits each>". The
// offset is the index in the frame buffer, so a host script can place every
// value without guessing the chunking.
#define SHOT_CHUNK 48
static void serial_dump(const char *name, const uint16_t *fb)
{
    static char line[16 + 8 + 10 + SHOT_CHUNK * 4 + 4];
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x += SHOT_CHUNK) {
            int n = snprintf(line, sizeof(line), "SHOT %s %d ", name,
                             y * SCREEN_W + x);
            for (int i = 0; i < SHOT_CHUNK; i++) {
                uint16_t px = RGB565_SWAP(fb[y * SCREEN_W + x + i]);
                line[n++] = "0123456789ABCDEF"[px >> 12];
                line[n++] = "0123456789ABCDEF"[(px >> 8) & 0xF];
                line[n++] = "0123456789ABCDEF"[(px >> 4) & 0xF];
                line[n++] = "0123456789ABCDEF"[px & 0xF];
            }
            line[n++] = '\n';
            line[n] = '\0';
            fwrite(line, 1, (size_t)n, stdout);
        }
        fflush(stdout);
        // One tick per row: a frame is a few hundred kilobytes, and without
        // yielding the idle task would starve and the watchdog would cut in.
        vTaskDelay(1);
    }
}
#endif // SCREENSHOT_SERIAL

void screenshot_capture(const char *name)
{
    const uint16_t *fb = display_get_framebuffer();
    if (fb == NULL) return;

    char base[12];
    shot_name(name ? name : "frame", base, sizeof(base));

    // Clock mode boots before the card is mounted; every other mode mounts it
    // itself. Debug only, so opening it here is the simplest way to work in
    // all modes.
    if (!sdcard_is_mounted() && sdcard_init() != ESP_OK) {
        ESP_LOGW(TAG, "no SD card, %s not written", base);
    } else {
        mkdir(SHOT_DIR, 0777); // exists after the first capture
        char path[48];
        snprintf(path, sizeof(path), SHOT_DIR "/%s.bmp", base);
        FILE *f = fopen(path, "wb");
        if (f == NULL) {
            ESP_LOGW(TAG, "cannot open %s", path);
        } else {
            // Static: the UI task's stack is small, and capture is single
            // threaded anyway.
            static uint8_t header[BMP_HEADER];
            static uint8_t row[BMP_ROW_BYTES];
            make_header(header, BMP_HEADER + (uint32_t)BMP_ROW_BYTES * SCREEN_H);
            bool ok = fwrite(header, 1, sizeof(header), f) == sizeof(header);
            // Bottom-up: the last screen row comes first in the file.
            for (int y = SCREEN_H - 1; ok && y >= 0; y--) {
                row_to_bmp(fb + (size_t)y * SCREEN_W, row);
                ok = fwrite(row, 1, sizeof(row), f) == sizeof(row);
            }
            if (fclose(f) != 0 || !ok) {
                ESP_LOGW(TAG, "write failed on %s", path);
            } else {
                ESP_LOGI(TAG, "wrote %s", path);
            }
        }
    }

#if SCREENSHOT_SERIAL
    serial_dump(base, fb);
#endif
}

#else // !SCREENSHOT_ENABLE

// Every call site can stay in place; the switch removes the whole body.
void screenshot_capture(const char *name)
{
    (void)name;
}

#endif // SCREENSHOT_ENABLE
