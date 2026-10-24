#include "slideshow.h"
#include "display.h"
#include "sdcard.h"
#include "touch.h"
#include "status.h"
#include "webserver.h"
#include "audio.h"
#include "clock.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "pins.h"
#include "esp_log.h"
#include "esp_jpeg_dec.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "features.h"
#include "settings.h"

#define MAX_SLIDE_BYTES (512 * 1024)
#define MAX_SLIDES 256

static const char *TAG = "slideshow";
// Shared RGB565 frame buffer, assigned in slideshow_run(). Both the slides
// and the clock overlay are drawn into it, which is what makes one buffer
// enough for the whole firmware.
static uint16_t *slide_fb = NULL;
static volatile bool advance_req = false;
static int64_t last_interaction = 0;
static esp_pm_lock_handle_t cpu_max_lock = NULL;
static TaskHandle_t main_task = NULL;
static esp_timer_handle_t wake_timer = NULL;
static int last_brightness = -1;
static int last_clock_min = -1;

//
// Wake-up plumbing: one one-shot esp_timer plus a task notification wakes the
// main loop at the next slide, minute boundary, backlight step or touch event.
//

static void wake_main_task(void)
{
    if (main_task) {
        xTaskNotifyGive(main_task);
    }
}

// Set when something changed that the current frame does not show yet (the
// setup indicator, for example): the modes redraw on the minute boundary, so
// a change like that needs its own pass through the loop.
static volatile bool force_redraw = false;

static bool take_force_redraw(void)
{
    if (!force_redraw) return false;
    force_redraw = false;
    return true;
}

void ui_force_redraw(void)
{
    force_redraw = true;
    wake_main_task();
}

static void wake_timer_cb(void *arg)
{
    wake_main_task();
}

// Microseconds until the next backlight dimming step, relative to now_us.
static int64_t next_backlight_step_us(int64_t now_us)
{
    int64_t idle_sec = now_us / 1000000LL - last_interaction;
    if (idle_sec < settings.backlight_dim_after_sec) {
        return (settings.backlight_dim_after_sec - idle_sec) * 1000000LL;
    }
    if (idle_sec < settings.backlight_deep_after_sec) {
        return (settings.backlight_deep_after_sec - idle_sec) * 1000000LL;
    }
    return INT64_MAX; // already at the deepest level
}

// Microseconds until the next wall-clock minute boundary.
static int64_t to_minute_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (60 - tv.tv_sec % 60) * 1000000LL - tv.tv_usec;
}

static void update_backlight(void)
{
    int64_t now = esp_timer_get_time() / 1000000LL;
    if (last_interaction == 0) last_interaction = now;
    int64_t idle = now - last_interaction;
    int brightness;
    if (idle > settings.backlight_deep_after_sec) {
        brightness = settings.backlight_deep_pct;
    } else if (idle > settings.backlight_dim_after_sec) {
        brightness = settings.backlight_dim_pct;
    } else {
        brightness = settings.backlight_full_pct;
    }
    // Only touch the LEDC peripheral when the level actually changes.
    if (brightness != last_brightness) {
        display_backlight_set_brightness(brightness);
        last_brightness = brightness;
    }
}

//
// Slide engine: shared by the slideshow and combined modes.
//

static char *slide_paths[MAX_SLIDES];
static size_t slide_count = 0;
static size_t slide_idx = 0;
static bool slide_active = false; // a slide is currently displayed
static int64_t slide_deadline_us = 0;

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
    snprintf(dirpath, sizeof(dirpath), "/sdcard%s", settings.slide_dir);
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

// Fills the shared buffer with a plain RGB565 color and pushes it to the
// panel; the byte swap for the panel happens here, once, so callers pass
// natural color values.
static void fill_color(uint16_t color)
{
    uint16_t px = RGB565_SWAP(color);
    for (size_t i = 0; i < 240 * 240; i++) slide_fb[i] = px;
    ui_status_draw(slide_fb);
    esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0, 240, 240, slide_fb);
}

// Decoder handle, opened on the first slide and kept for the lifetime of the
// firmware: reopening it per image costs heap churn and setup time. A failed
// decode closes the handle, so the next slide starts from a clean state.
static jpeg_dec_handle_t jpeg_dec = NULL;

static bool jpeg_dec_ready(void)
{
    if (jpeg_dec) return true;
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_BE; // panel wants MSB first
    if (jpeg_dec_open(&config, &jpeg_dec) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "cannot open the JPEG decoder");
        jpeg_dec = NULL;
        return false;
    }
    return true;
}

static void jpeg_dec_drop(void)
{
    if (jpeg_dec) {
        jpeg_dec_close(jpeg_dec);
        jpeg_dec = NULL;
    }
}

static esp_err_t show_slide(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return ESP_FAIL;
    if (st.st_size <= 0 || st.st_size > MAX_SLIDE_BYTES) return ESP_FAIL;

    int64_t t0 = esp_timer_get_time();
    uint8_t *buf = malloc(st.st_size);
    if (!buf) return ESP_ERR_NO_MEM;
    FILE *f = fopen(path, "rb");
    if (!f) { free(buf); return ESP_FAIL; }
    size_t rd = fread(buf, 1, st.st_size, f);
    fclose(f);
    if (rd != (size_t)st.st_size) { free(buf); return ESP_FAIL; }
    int64_t t1 = esp_timer_get_time();

    if (!jpeg_dec_ready()) { free(buf); return ESP_FAIL; }

    jpeg_dec_io_t io = {0};
    jpeg_dec_header_info_t info = {0};
    io.inbuf = buf;
    io.inbuf_len = (int)rd;
    jpeg_error_t jerr = jpeg_dec_parse_header(jpeg_dec, &io, &info);
    if (jerr != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "cannot parse %s: %d", path, (int)jerr);
        free(buf);
        jpeg_dec_drop();
        return ESP_FAIL;
    }
    if ((size_t)info.width * info.height * sizeof(uint16_t) >
        240 * 240 * sizeof(uint16_t)) {
        ESP_LOGE(TAG, "%s is %ux%u, larger than the panel", path,
                 (unsigned)info.width, (unsigned)info.height);
        free(buf);
        return ESP_FAIL;
    }
    io.outbuf = (uint8_t *)slide_fb;
    io.out_size = info.width * info.height * sizeof(uint16_t);
    jerr = jpeg_dec_process(jpeg_dec, &io);
    free(buf);
    if (jerr != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "decode failed %s: %d", path, (int)jerr);
        jpeg_dec_drop();
        return ESP_FAIL;
    }
    int64_t t2 = esp_timer_get_time();
    ui_status_draw(slide_fb);
    esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0, info.width, info.height, slide_fb);
    int64_t t3 = esp_timer_get_time();
    ESP_LOGI(TAG, "slide %s: %u B, read %lld ms, decode %lld ms, blit %lld ms, heap %u",
             path, (unsigned)rd,
             (long long)((t1 - t0) / 1000), (long long)((t2 - t1) / 1000),
             (long long)((t3 - t2) / 1000),
             (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

// Mount and scan the card if needed; shows the error screen (red = no card,
// yellow = no slides) and returns false when slides are not usable.
static bool ensure_slides(void)
{
    if (!sdcard_is_mounted()) {
        if (sdcard_init() != ESP_OK) {
            ESP_LOGW(TAG, "no SD card");
            fill_color(0xF800); // red
            vTaskDelay(pdMS_TO_TICKS(2000));
            return false;
        }
    }
    if (slide_count == 0) {
        scan_slides();
        if (slide_count == 0) {
            ESP_LOGW(TAG, "no slides");
            fill_color(0xFFE0); // yellow
            vTaskDelay(pdMS_TO_TICKS(2000));
            return false;
        }
    }
    return true;
}

static void show_current_slide(void)
{
    esp_err_t r = show_slide(slide_paths[slide_idx]);
    if (r != ESP_OK) {
        fill_color(0xF800); // red
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    slide_deadline_us = esp_timer_get_time() +
                        (int64_t)settings.slide_interval_ms * 1000;
    slide_active = true;
}

// Draw a slide if one is due (first draw, tap advance or interval elapsed).
// Returns true when the screen was redrawn with a slide.
static bool slideshow_step(void)
{
    if (!ensure_slides()) return false;
    if (!slide_active) {
        show_current_slide();
        return true;
    }
    if (advance_req || esp_timer_get_time() >= slide_deadline_us) {
        advance_req = false;
        slide_idx = (slide_idx + 1) % slide_count;
        show_current_slide();
        return true;
    }
    return false;
}

//
// UI mode interface ("base class"): every mode implements these callbacks.
//
// - enter/exit run in the main task when a mode becomes active/is replaced;
//   enter must draw the mode's initial state so switches are always visible.
// - on_tap/on_long_press run in the touch task.
// - on_wake runs in the main task on every timer/touch wake-up.
// - next_wake_us tells the mode manager when the mode wants to be woken next.
//
// Adding a mode (e.g. the upcoming weather station): implement this struct,
// link it into the cycle via the next field and pass it to request_mode().
//

typedef struct ui_mode ui_mode_t;
struct ui_mode {
    const char *name;
    const ui_mode_t *next;    // target of a long press (the mode cycle)
    int enter_tone_hz;        // feedback tone when entering this mode
    void (*enter)(void);
    void (*exit)(void);       // may be NULL
    void (*on_tap)(void);     // may be NULL
    void (*on_long_press)(void);
    void (*on_wake)(void);
    int64_t (*next_wake_us)(int64_t now_us);
};

static const ui_mode_t clock_mode;
static const ui_mode_t slideshow_mode;
static const ui_mode_t combined_mode;

static const ui_mode_t *active_mode = NULL;
static const ui_mode_t *pending_mode = NULL;

// Request a mode switch from any task; the main loop performs it on wake.
static void request_mode(const ui_mode_t *mode)
{
    ESP_LOGI(TAG, "mode: %s", mode->name);
    pending_mode = mode;
    wake_main_task();
}

// Default long-press behavior: cycle to the mode's next in the cycle.
static void mode_on_long_press(void)
{
    last_interaction = esp_timer_get_time() / 1000000LL;
    display_backlight_set_brightness(settings.backlight_full_pct);
    last_brightness = settings.backlight_full_pct;
    const ui_mode_t *target = active_mode->next;
    audio_play_tone(target->enter_tone_hz, 80);
    request_mode(target);
}

static void on_touch_tap(void)
{
    last_interaction = esp_timer_get_time() / 1000000LL;
    display_backlight_set_brightness(settings.backlight_full_pct);
    last_brightness = settings.backlight_full_pct;
    if (active_mode && active_mode->on_tap) {
        active_mode->on_tap();
    }
    wake_main_task();
}

static void on_long_press(void)
{
    if (active_mode && active_mode->on_long_press) {
        active_mode->on_long_press();
    }
    wake_main_task();
}

// Setup gesture: finger at one spot for 5 s. Starts the setup page, or
// stops it again when it already runs; the red dot and the address on the
// screen follow whatever the server reports.
static void on_setup_gesture(void)
{
    last_interaction = esp_timer_get_time() / 1000000LL;
    display_backlight_set_brightness(settings.backlight_full_pct);
    last_brightness = settings.backlight_full_pct;
    audio_play_tone(1046, 120); // the gesture was taken, even if WiFi fails
    ESP_LOGI(TAG, "setup gesture, %s the setup page",
             web_is_running() ? "stopping" : "starting");
    web_toggle();
}

// Keep maximum CPU frequency only while slideshow work runs; the clock mode
// may throttle down and light-sleep.
static void apply_power_for_mode(bool clock_active)
{
    if (!cpu_max_lock) return;
    if (clock_active) {
        esp_pm_lock_release(cpu_max_lock);
    } else {
        esp_pm_lock_acquire(cpu_max_lock);
    }
}

//
// Mode: clock only (cycle position 1)
//

static void clock_enter(void)
{
    apply_power_for_mode(true);
    last_clock_min = -1;
    clock_render(NULL); // black background, draws immediately
    struct tm tm;
    time_t now = time(NULL);
    localtime_r(&now, &tm);
    last_clock_min = tm.tm_min;
}

static void clock_on_tap(void)
{
    // Nothing to advance in clock mode; the wrapper still resets dimming.
}

static void clock_on_wake(void)
{
    struct tm tm;
    time_t now = time(NULL);
    localtime_r(&now, &tm);
    if (take_force_redraw() || tm.tm_min != last_clock_min) {
        clock_render(NULL);
        last_clock_min = tm.tm_min;
    }
}

static int64_t clock_next_wake_us(int64_t now_us)
{
    int64_t wake_us = to_minute_us();
    int64_t to_dim_us = next_backlight_step_us(now_us);
    if (to_dim_us < wake_us) wake_us = to_dim_us;
    return wake_us;
}

//
// Mode: slideshow only (cycle position 2)
//

static void slideshow_enter(void)
{
    apply_power_for_mode(false);
    advance_req = false;
    // Redraw unconditionally: the clock mode may have blanked the shared
    // frame buffer, so the buffered slide cannot be trusted anymore.
    slide_active = false;
    slideshow_step(); // draws the current (or next due) slide right away
}

static void slideshow_on_tap(void)
{
    if (slide_count == 0) return; // error screen, nothing to advance
    audio_play_tone(660, 60);
    advance_req = true;
}

static void slideshow_on_wake(void)
{
    if (take_force_redraw()) slide_active = false; // redraw with the overlay
    slideshow_step();
}

static int64_t slideshow_next_wake_us(int64_t now_us)
{
    int64_t wake_us = slide_active ? (slide_deadline_us - now_us) : 2000000LL;
    int64_t to_dim_us = next_backlight_step_us(now_us);
    if (to_dim_us < wake_us) wake_us = to_dim_us;
    return wake_us;
}

//
// Mode: clock + slideshow, combined (cycle position 3)
//

static void combined_enter(void)
{
    apply_power_for_mode(false);
    advance_req = false;
    slide_active = false; // same reason as in slideshow_enter
    slideshow_step();
    // slide_fb holds the latest slide (or the error fill); draw over it.
    last_clock_min = -1;
    clock_render(slide_fb);
    struct tm tm;
    time_t now = time(NULL);
    localtime_r(&now, &tm);
    last_clock_min = tm.tm_min;
}

static void combined_on_wake(void)
{
    bool redrawn = slideshow_step();
    if (take_force_redraw() && !redrawn) {
        slide_active = false;
        redrawn = slideshow_step();
    }
    struct tm tm;
    time_t now = time(NULL);
    localtime_r(&now, &tm);
    if (redrawn || tm.tm_min != last_clock_min) {
        // The overlay writes into the shared buffer, so the slide pixels
        // under the clock are gone. Without a fresh slide, decode it again
        // first to avoid compositing the clock over its own leftovers.
        if (!redrawn && slide_count > 0) {
            show_slide(slide_paths[slide_idx]);
        }
        clock_render(slide_fb);
        last_clock_min = tm.tm_min;
    }
}

static int64_t combined_next_wake_us(int64_t now_us)
{
    int64_t wake_us = slideshow_next_wake_us(now_us);
    int64_t to_min_us = to_minute_us();
    if (to_min_us < wake_us) wake_us = to_min_us;
    return wake_us;
}

//
// Mode instances. Long-press cycle: clock -> slideshow -> combined -> clock.
// The device boots into clock mode.
//

static const ui_mode_t clock_mode = {
    .name = "clock",
    .next = &slideshow_mode,
    .enter_tone_hz = 880,
    .enter = clock_enter,
    .exit = NULL,
    .on_tap = clock_on_tap,
    .on_long_press = mode_on_long_press,
    .on_wake = clock_on_wake,
    .next_wake_us = clock_next_wake_us,
};

static const ui_mode_t slideshow_mode = {
    .name = "slideshow",
    .next = &combined_mode,
    .enter_tone_hz = 440,
    .enter = slideshow_enter,
    .exit = NULL,
    .on_tap = slideshow_on_tap,
    .on_long_press = mode_on_long_press,
    .on_wake = slideshow_on_wake,
    .next_wake_us = slideshow_next_wake_us,
};

static const ui_mode_t combined_mode = {
    .name = "clock+slideshow",
    .next = &clock_mode,
    .enter_tone_hz = 988,
    .enter = combined_enter,
    .exit = NULL,
    .on_tap = slideshow_on_tap,
    .on_long_press = mode_on_long_press,
    .on_wake = combined_on_wake,
    .next_wake_us = combined_next_wake_us,
};

//
// Mode manager main loop.
//

void slideshow_run(void)
{
    slide_fb = display_get_framebuffer();
    clock_init();
    last_interaction = esp_timer_get_time() / 1000000LL;
#if CLOCK_DEEP_SLEEP
    // A timer wake-up continued a deep sleep: either the sleep window just
    // ended (the clock reappears dimmed) or it still lasts and the loop sends
    // the board straight back to sleep. Both want the backlight schedule to
    // start at its dimmest level instead of lighting the display up again.
    if (esp_sleep_get_wakeup_causes() & (1 << ESP_SLEEP_WAKEUP_TIMER)) {
        last_interaction -= settings.backlight_deep_after_sec + 1;
    }
#endif

    // Power management: keep max CPU frequency while slideshow work is active.
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "slideshow", &cpu_max_lock);

    // Timer used to wake the loop at the next slide, minute, backlight step
    // or touch event.
    main_task = xTaskGetCurrentTaskHandle();
    const esp_timer_create_args_t timer_args = {
        .callback = wake_timer_cb,
        .name = "ui_wake",
    };
    esp_timer_create(&timer_args, &wake_timer);

    // Boot into clock mode (cycle position 1).
    active_mode = &clock_mode;

    touch_init(on_touch_tap);
    touch_set_long_press_cb(on_long_press);
    touch_set_setup_cb(on_setup_gesture);
    active_mode->enter();

    while (1) {
        // Perform a pending mode switch before any other work, so the new
        // mode's enter() redraw makes the change visible immediately.
        if (pending_mode) {
            const ui_mode_t *next = pending_mode;
            pending_mode = NULL;
            if (active_mode->exit) active_mode->exit();
            active_mode = next;
            active_mode->enter();
        }

#if CLOCK_DEEP_SLEEP
        // Clock mode only, and only inside the sleep window: once the
        // backlight schedule has run down, deep sleep until the window ends
        // (or a touch wakes the board). The check runs before on_wake() and
        // update_backlight(), so a wake-up that finds the window still active
        // stays completely dark - no redraw, no backlight. The touch line must
        // be idle-high, otherwise the ext0 wake would fire right away and
        // loop the boot.
        if (settings.clock_deep_sleep_enable && active_mode == &clock_mode &&
            clock_deep_sleep_window_active() &&
            esp_timer_get_time() / 1000000LL - last_interaction >
                settings.backlight_deep_after_sec &&
            gpio_get_level((gpio_num_t)PIN_TP_INT) != 0) {
            int64_t sleep_us = clock_deep_sleep_window_end_us();
            if (sleep_us > 1000000) {
                ESP_LOGI(TAG, "deep sleep, window ends in %lld s",
                         (long long)(sleep_us / 1000000));
                display_deep_sleep_prepare();
                // The touch controller signals with open-drain low pulses;
                // keep the line pulled up so it cannot float into a false
                // wake-up.
                gpio_set_pull_mode((gpio_num_t)PIN_TP_INT, GPIO_PULLUP_ONLY);
                esp_sleep_enable_timer_wakeup((uint64_t)sleep_us);
                esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_TP_INT, 0);
                esp_deep_sleep_start();
            }
        }
#endif

        active_mode->on_wake();
        update_backlight();

        int64_t wake_us = active_mode->next_wake_us(esp_timer_get_time());
        if (wake_us < 10000) wake_us = 10000;     // avoid a busy spin
        if (wake_us > 60000000) wake_us = 60000000; // cap the safety timeout

        esp_timer_start_once(wake_timer, wake_us);
        // Woken by the timer or by touch; timeout is a safety net.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wake_us / 1000 + 2000));
        esp_timer_stop(wake_timer);
    }
}
