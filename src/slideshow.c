#include "slideshow.h"
#include "display.h"
#include "sdcard.h"
#include "touch.h"
#include "audio.h"
#include "clock.h"
#include "esp_timer.h"
#include "pins.h"
#include "esp_log.h"
#include "jpeg_decoder.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#if __has_include("config.h")
#include "config.h"
#endif

#ifndef SLIDE_INTERVAL_MS
#define SLIDE_INTERVAL_MS 10000
#endif
#ifndef SLIDE_DIR
#define SLIDE_DIR "/slides"
#endif

#define MAX_SLIDE_BYTES (512 * 1024)
#define MAX_SLIDES 256
#define DIM_AFTER_SEC 15
#define DIM_BRIGHTNESS 20

static const char *TAG = "slideshow";
static uint16_t slide_fb[240 * 240];
static volatile bool advance_req = false;
static volatile int ui_mode = 0; // 0 slideshow, 1 clock
static int64_t last_interaction = 0;

static char *slide_paths[MAX_SLIDES];
static size_t slide_count = 0;

static void update_backlight(void)
{
    int64_t now = esp_timer_get_time() / 1000000LL;
    if (last_interaction == 0) last_interaction = now;
    int64_t idle = now - last_interaction;
    if (idle > 60) {
        display_backlight_set_brightness(5);
    } else if (idle > DIM_AFTER_SEC) {
        display_backlight_set_brightness(20);
    } else {
        display_backlight_set_brightness(100);
    }
}

static void on_touch_tap(void)
{
    last_interaction = esp_timer_get_time() / 1000000LL;
    display_backlight_set_brightness(100);
    if (ui_mode == 0) {
        advance_req = true;
    }
}

static void on_long_press(void)
{
    last_interaction = esp_timer_get_time() / 1000000LL;
    display_backlight_set_brightness(100);
    ui_mode = ui_mode ? 0 : 1;
    audio_play_tone(ui_mode ? 880 : 440, 80);
}

static int cmp_str(const void *a, const void *b)
{
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcmp(sa, sb);
}

static bool is_jpeg(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) return false;
    const char *ext = name + len - 4;
    if (strcasecmp(ext, ".jpg") == 0) return true;
    if (len >= 5 && strcasecmp(name + len - 5, ".jpeg") == 0) return true;
    return false;
}

static void free_slides(void)
{
    for (size_t i = 0; i < slide_count; i++) {
        free(slide_paths[i]);
        slide_paths[i] = NULL;
    }
    slide_count = 0;
}

static void scan_slides(void)
{
    free_slides();
    char dirpath[128];
    snprintf(dirpath, sizeof(dirpath), "/sdcard%s", SLIDE_DIR);
    DIR *d = opendir(dirpath);
    if (!d) {
        ESP_LOGW(TAG, "cannot open %s", dirpath);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && slide_count < MAX_SLIDES) {
        if (!is_jpeg(e->d_name)) continue;
        size_t plen = strlen(dirpath) + 1 + strlen(e->d_name) + 1;
        char *p = malloc(plen);
        if (!p) continue;
        snprintf(p, plen, "%s/%s", dirpath, e->d_name);
        slide_paths[slide_count++] = p;
    }
    closedir(d);
    qsort(slide_paths, slide_count, sizeof(char *), cmp_str);
    ESP_LOGI(TAG, "found %u slides", (unsigned)slide_count);
}

static void fill_color(uint16_t color)
{
    for (size_t i = 0; i < 240 * 240; i++) slide_fb[i] = color;
    esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0, 240, 240, slide_fb);
}

static esp_err_t show_slide(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return ESP_FAIL;
    if (st.st_size <= 0 || st.st_size > MAX_SLIDE_BYTES) return ESP_FAIL;

    uint8_t *buf = malloc(st.st_size);
    if (!buf) return ESP_ERR_NO_MEM;
    FILE *f = fopen(path, "rb");
    if (!f) { free(buf); return ESP_FAIL; }
    size_t rd = fread(buf, 1, st.st_size, f);
    fclose(f);
    if (rd != (size_t)st.st_size) { free(buf); return ESP_FAIL; }

    esp_jpeg_image_cfg_t cfg = {
        .indata = buf,
        .indata_size = (uint32_t)rd,
        .outbuf = (uint8_t *)slide_fb,
        .outbuf_size = 240 * 240 * sizeof(uint16_t),
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = { .swap_color_bytes = 1 },
    };
    esp_jpeg_image_output_t out = {0};
    esp_err_t ret = esp_jpeg_decode(&cfg, &out);
    free(buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "decode failed %s: %s", path, esp_err_to_name(ret));
        return ret;
    }
    esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0, out.width, out.height, slide_fb);
    return ESP_OK;
}

void slideshow_run(void)
{
    clock_init();
    last_interaction = esp_timer_get_time() / 1000000LL;
    touch_init(on_touch_tap);
    touch_set_long_press_cb(on_long_press);

    while (1) {
        if (ui_mode == 1) {
            // Clock mode
            while (ui_mode == 1) {
                update_backlight();
                clock_render();
                for (int i = 0; i < 10 && ui_mode == 1; i++) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
            fill_color(0x0000);
            continue;
        }

        // Slideshow mode
        if (!sdcard_is_mounted()) {
            if (sdcard_init() != ESP_OK) {
                ESP_LOGW(TAG, "no SD card");
                fill_color(0xF800); // red
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        scan_slides();
        if (slide_count == 0) {
            ESP_LOGW(TAG, "no slides");
            fill_color(0xFFE0); // yellow
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        size_t idx = 0;
        while (sdcard_is_mounted() && ui_mode == 0) {
            update_backlight();
            advance_req = false;
            esp_err_t r = show_slide(slide_paths[idx]);
            if (r != ESP_OK) {
                fill_color(0xF800);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }

            int waited = 0;
            while (waited < SLIDE_INTERVAL_MS && !advance_req && sdcard_is_mounted() && ui_mode == 0) {
                update_backlight();
                vTaskDelay(pdMS_TO_TICKS(50));
                waited += 50;
            }
            if (advance_req && ui_mode == 0) {
                audio_play_tone(660, 60);
            }
            if (!sdcard_is_mounted() || ui_mode != 0) break;
            idx = (idx + 1) % slide_count;
        }
        free_slides();
    }
}