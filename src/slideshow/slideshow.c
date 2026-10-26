#include "slideshow.h"
#include "display.h"
#include "sdcard.h"
#include "touch.h"
#include "status.h"
#include "webserver.h"
#include "net.h"
#include "audio.h"
#include "alarm.h"
#include "clock.h"
#include "weather.h"
#include "radio.h"
#include "esp_heap_caps.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "pins.h"
#include "esp_log.h"
#include "esp_jpeg_dec.h"
#include "esp_lcd_panel_ops.h"
#include "driver/usb_serial_jtag.h"
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
#include "screenshot.h"
#include "osd.h"

#define MAX_SLIDE_BYTES (512 * 1024)
#define MAX_SLIDES 256
// One volume step, and how much finger travel buys one. Twenty steps span the
// whole range, so a slide across the panel is worth about a quarter of it,
// while a short flick still nudges it by one - the slide is the natural gesture
// on a screen this small, and every extra gesture is one more chance to hit
// something else by accident.
#define VOLUME_STEP_PCT 5
#define VOLUME_STEP_PX 40
#define VOLUME_MAX_STEPS 5

static const char *TAG = "slideshow";
// Shared RGB565 frame buffer, assigned in slideshow_run(). Both the slides
// and the clock overlay are drawn into it, so one internal buffer is enough
// for the whole firmware; combined mode keeps a clock-free slide copy in
// PSRAM next to the fade frame, see clean_buf below.
static uint16_t *slide_fb = NULL;
static volatile bool advance_req = false;
static int64_t last_interaction = 0;
static esp_pm_lock_handle_t cpu_max_lock = NULL;
static esp_pm_lock_handle_t usb_pm_lock = NULL;
static bool usb_pm_hold = false;
static TaskHandle_t main_task = NULL;
static esp_timer_handle_t wake_timer = NULL;
static int last_brightness = -1;
static int last_clock_min = -1;
// PSRAM copy of the slide without the clock overlay. Combined mode needs the
// plain image again when only the minute changes: the overlay painted over the
// slide pixels in the shared frame buffer, and decoding the JPEG again just to
// repaint them would cost a full SD read and decode every minute.
static uint16_t *clean_buf = NULL;
static bool clean_valid = false;
static bool clean_warned = false;
// True while the clock+slideshow mode is active: slides are composited with
// the clock before they are pushed, so a crossfade keeps the clock static.
static bool combined_overlay = false;

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

// The deepest backlight step can be a dark screen (the level is a percent
// value, 0 is off). That only belongs to the night window, where the clock
// mode goes to deep sleep anyway and a touch is the only thing that should
// light the display. Outside the window the schedule stops at the dim level:
// a clock that vanishes in the middle of the day looks like a dead device,
// and the wake-up at the end of the window would leave it dark all day.
static bool backlight_blank_allowed(void)
{
#if CLOCK_DEEP_SLEEP
    return settings.clock_deep_sleep_enable && clock_deep_sleep_window_active();
#else
    return false;
#endif
}

// Microseconds until the next backlight dimming step, relative to now_us.
static int64_t next_backlight_step_us(int64_t now_us)
{
    int64_t idle_sec = now_us / 1000000LL - last_interaction;
    if (idle_sec < settings.backlight_dim_after_sec) {
        return (settings.backlight_dim_after_sec - idle_sec) * 1000000LL;
    }
    if (idle_sec < settings.backlight_deep_after_sec && backlight_blank_allowed()) {
        return (settings.backlight_deep_after_sec - idle_sec) * 1000000LL;
    }
    return INT64_MAX; // already at the deepest level this time of day allows
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
    if (idle >= settings.backlight_deep_after_sec && backlight_blank_allowed()) {
        brightness = settings.backlight_deep_pct;
    } else if (idle >= settings.backlight_dim_after_sec) {
        brightness = settings.backlight_dim_pct;
    } else {
        brightness = settings.backlight_full_pct;
    }
    // Only touch the LEDC peripheral when the level actually changes.
    if (brightness != last_brightness) {
        display_backlight_set_brightness(brightness);
        ESP_LOGI(TAG, "backlight %d%% after %lld s idle", brightness,
                 (long long)idle);
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
    clean_valid = false; // the clean copy no longer matches the screen
    if (combined_overlay) {
        clock_compose(slide_fb); // error screens keep the clock on top
    } else {
        ui_status_draw(slide_fb);
    }
    esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0, 240, 240, slide_fb);
}

// CPU at full speed: taken around the work that needs it (SD access, decode,
// DMA setup) and released again, so the power manager can drop the clock and
// enter automatic light sleep while nothing happens.
static bool cpu_max_held = false;

static void cpu_max_hold(bool hold)
{
    if (!cpu_max_lock || hold == cpu_max_held) return;
    if (hold) {
        esp_pm_lock_acquire(cpu_max_lock);
    } else {
        esp_pm_lock_release(cpu_max_lock);
    }
    cpu_max_held = hold;
}

// Guard for the image work. With SLIDE_PM_LOW_POWER the lock is taken per
// image and dropped in between; without it the slide modes hold it for their
// whole lifetime and this does nothing.
static void slide_work_hold(bool hold)
{
#if SLIDE_PM_LOW_POWER
    cpu_max_hold(hold);
#else
    (void)hold;
#endif
}

// Decoder handle, opened on the first slide of a slide mode and released when
// the clock mode takes over. Reopening per image would cost heap churn and
// setup time, so it stays open across images. A failed decode closes it, so
// the next image starts from a clean state.
static jpeg_dec_handle_t jpeg_dec = NULL;

// One input buffer for all images, grown when a bigger file shows up. It is
// reused instead of allocating per image, so thousands of image changes cannot
// fragment the heap, and it is released together with the decoder whenever a
// slide mode ends.
static uint8_t *jpeg_buf = NULL;
static size_t jpeg_buf_cap = 0;

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

// Grows the shared input buffer to at least size bytes and returns it.
static uint8_t *jpeg_buf_get(size_t size)
{
    if (size > jpeg_buf_cap) {
        uint8_t *p = realloc(jpeg_buf, size);
        if (!p) return NULL;
        jpeg_buf = p;
        jpeg_buf_cap = size;
    }
    return jpeg_buf;
}

// Crossfade between two images: the new one is decoded into a PSRAM frame
// while the previous one stays in the internal frame buffer, then both are
// mixed band by band and pushed to the panel. Without PSRAM the setting is
// ignored and the image is drawn in one go, as before.
#define FADE_BAND_LINES 16
#define FADE_STEP_US 57000 // measured: SPI plus blend for one step

static uint16_t *fade_buf = NULL;    // PSRAM, one full frame
static bool fade_warned = false;
static uint16_t shown_w = 0;         // size of the image on the panel
static uint16_t shown_h = 0;
static uint16_t fade_band[240 * FADE_BAND_LINES] __attribute__((aligned(16)));

// Mixes two panel-order RGB565 pixels: alpha 0 returns a, 256 returns b.
static inline uint16_t blend565(uint16_t a, uint16_t b, unsigned alpha)
{
    a = RGB565_SWAP(a);
    b = RGB565_SWAP(b);
    unsigned inv = 256 - alpha;
    unsigned r = (((a >> 11) & 0x1F) * inv + ((b >> 11) & 0x1F) * alpha) >> 8;
    unsigned g = (((a >> 5) & 0x3F) * inv + ((b >> 5) & 0x3F) * alpha) >> 8;
    unsigned bl = ((a & 0x1F) * inv + (b & 0x1F) * alpha) >> 8;
    return RGB565_SWAP((uint16_t)((r << 11) | (g << 5) | bl));
}

// Allocates the second frame in PSRAM on first use. Returns NULL when the
// build or the board has none, which turns the fade off for good.
static uint16_t *fade_buf_get(void)
{
    if (fade_buf) return fade_buf;
    fade_buf = heap_caps_aligned_alloc(64, 240 * 240 * sizeof(uint16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fade_buf && !fade_warned) {
        ESP_LOGW(TAG, "no PSRAM frame buffer, crossfade stays off");
        fade_warned = true;
    }
    return fade_buf;
}

// Allocates the clock-free slide copy in PSRAM on first use. Returns NULL when
// the build or the board has none; combined mode then falls back to decoding
// the slide again on every minute change.
static uint16_t *clean_buf_get(void)
{
    if (clean_buf) return clean_buf;
    clean_buf = heap_caps_aligned_alloc(64, 240 * 240 * sizeof(uint16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!clean_buf && !clean_warned) {
        ESP_LOGW(TAG, "no PSRAM for the clean slide copy, minute updates re-decode");
        clean_warned = true;
    }
    return clean_buf;
}

// Draws one alpha step of the transition, band by band.
static void fade_step(const uint16_t *old_img, const uint16_t *new_img,
                      int width, int height, unsigned alpha)
{
    for (int y = 0; y < height; y += FADE_BAND_LINES) {
        int lines = height - y;
        if (lines > FADE_BAND_LINES) lines = FADE_BAND_LINES;
        const uint16_t *a = old_img + (size_t)y * width;
        const uint16_t *b = new_img + (size_t)y * width;
        for (int i = 0; i < lines * width; i++) {
            fade_band[i] = blend565(a[i], b[i], alpha);
        }
        esp_lcd_panel_draw_bitmap(display_get_panel(), 0, y, width, y + lines,
                                  fade_band);
    }
}

// Runs the whole transition and returns its duration in milliseconds.
static int64_t fade_transition(const uint16_t *old_img, const uint16_t *new_img,
                               int width, int height)
{
    int steps = (int)((settings.slide_fade_ms * 1000) / FADE_STEP_US);
    if (steps < 2) steps = 2;
    if (steps > 40) steps = 40;
    int64_t t0 = esp_timer_get_time();
    for (int s = 1; s <= steps; s++) {
        fade_step(old_img, new_img, width, height, (unsigned)(s * 256 / steps));
    }
    return (esp_timer_get_time() - t0) / 1000;
}

// Both slide modes call this on exit: the decoder and the input buffer are
// only needed while images are on screen, so the clock mode gets the memory
// back (roughly 14 kB with typical file sizes).
static void release_slide_resources(void)
{
    jpeg_dec_drop();
    free(jpeg_buf);
    jpeg_buf = NULL;
    jpeg_buf_cap = 0;
    heap_caps_free(fade_buf);
    fade_buf = NULL;
    heap_caps_free(clean_buf);
    clean_buf = NULL;
    clean_valid = false;
    combined_overlay = false;
    shown_w = 0;
    shown_h = 0;
    ESP_LOGI(TAG, "released decoder and input buffer, heap %u",
             (unsigned)esp_get_free_heap_size());
}

static esp_err_t show_slide_now(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return ESP_FAIL;
    if (st.st_size <= 0 || st.st_size > MAX_SLIDE_BYTES) return ESP_FAIL;

    int64_t t0 = esp_timer_get_time();
    uint8_t *buf = jpeg_buf_get(st.st_size);
    if (!buf) return ESP_ERR_NO_MEM;
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_FAIL;
    size_t rd = fread(buf, 1, st.st_size, f);
    fclose(f);
    if (rd != (size_t)st.st_size) return ESP_FAIL;
    int64_t t1 = esp_timer_get_time();

    if (!jpeg_dec_ready()) return ESP_FAIL;

    jpeg_dec_io_t io = {0};
    jpeg_dec_header_info_t info = {0};
    io.inbuf = buf;
    io.inbuf_len = (int)rd;
    jpeg_error_t jerr = jpeg_dec_parse_header(jpeg_dec, &io, &info);
    if (jerr != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "cannot parse %s: %d", path, (int)jerr);
        jpeg_dec_drop();
        return ESP_FAIL;
    }
    if ((size_t)info.width * info.height * sizeof(uint16_t) >
        240 * 240 * sizeof(uint16_t)) {
        ESP_LOGE(TAG, "%s is %ux%u, larger than the panel", path,
                 (unsigned)info.width, (unsigned)info.height);
        return ESP_FAIL;
    }
    // With a fade the new image lands in PSRAM, so the frame buffer keeps the
    // image that is currently on the panel.
    bool fade = settings.slide_fade_ms > 0 && shown_w > 0 && shown_h > 0 &&
                fade_buf_get() != NULL;
    uint16_t *dst = fade ? fade_buf : slide_fb;
    io.outbuf = (uint8_t *)dst;
    io.out_size = info.width * info.height * sizeof(uint16_t);
    jerr = jpeg_dec_process(jpeg_dec, &io);
    if (jerr != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "decode failed %s: %d", path, (int)jerr);
        jpeg_dec_drop();
        return ESP_FAIL;
    }
    int64_t t2 = esp_timer_get_time();
    int64_t blit_ms = 0;
    int64_t fade_ms = 0;
    size_t img_bytes = (size_t)info.width * info.height * sizeof(uint16_t);
    if (combined_overlay) {
        // Keep a clock-free copy before the overlay is drawn into `dst`: the
        // minute refresh restores the slide from it instead of decoding again.
        uint16_t *clean = clean_buf_get();
        if (clean) {
            memcpy(clean, dst, img_bytes);
            clean_valid = true;
        } else {
            clean_valid = false;
        }
        // Compose the clock into the new frame as well. Pixels that do not
        // change are then identical in both frames, and blend565(a, a, alpha)
        // leaves them untouched: only the slide itself crossfades.
        clock_compose(dst);
    } else {
        ui_status_draw(dst);
    }
    if (fade) {
        int w = info.width < shown_w ? info.width : shown_w;
        int h = info.height < shown_h ? info.height : shown_h;
        fade_ms = fade_transition(slide_fb, dst, w, h);
        // The last step ends on the new image: keep it in the frame buffer so
        // the next transition starts from what the panel shows.
        memcpy(slide_fb, dst, img_bytes);
        if (info.width != shown_w || info.height != shown_h) {
            // Sizes differ, the blend only covered the common area.
            esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0, info.width,
                                      info.height, slide_fb);
        }
    } else {
        esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0, info.width, info.height, slide_fb);
    }
    shown_w = info.width;
    shown_h = info.height;
    int64_t t3 = esp_timer_get_time();
    blit_ms = fade ? 0 : (t3 - t2) / 1000;
    ESP_LOGI(TAG, "slide %s: %u B, read %lld ms, decode %lld ms, blit %lld ms, fade %lld ms, heap %u",
             path, (unsigned)rd,
             (long long)((t1 - t0) / 1000), (long long)((t2 - t1) / 1000),
             (long long)blit_ms, (long long)fade_ms,
             (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

// The image work needs the CPU at full speed and must not be interrupted by a
// light sleep: the SD card read below blocks for milliseconds and the SDMMC
// driver takes no power management lock.
static esp_err_t show_slide(const char *path)
{
    slide_work_hold(true);
    esp_err_t r = show_slide_now(path);
    slide_work_hold(false);
    return r;
}

// Mount and scan the card if needed; shows the error screen (red = no card,
// yellow = no slides) and returns false when slides are not usable. The same
// power management rule applies as in show_slide() around the card access; the
// waits for the next retry stay outside it so the chip can sleep.
static bool ensure_slides(void)
{
    if (!sdcard_is_mounted()) {
        slide_work_hold(true);
        esp_err_t r = sdcard_init();
        slide_work_hold(false);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "no SD card");
            fill_color(0xF800); // red
            vTaskDelay(pdMS_TO_TICKS(2000));
            return false;
        }
    }
    if (slide_count == 0) {
        slide_work_hold(true);
        scan_slides();
        slide_work_hold(false);
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
// - on_tap gets the point the finger landed on (modes may draw buttons),
//   on_long_press runs without one.
// - on_wake runs in the main task on every timer/touch wake-up.
// - next_wake_us tells the mode manager when the mode wants to be woken next.
//
// Adding a mode (e.g. the weather station): implement this struct,
// link it into the cycle via the next field and pass it to request_mode().
// Out of the box the weather mode in weather.c is the example; WEATHER_ENABLE
// in features.h decides whether it joins the cycle.
//

typedef struct ui_mode ui_mode_t;
struct ui_mode {
    const char *name;
    const ui_mode_t *next;    // target of a long press (the mode cycle)
    int enter_tone_hz;        // feedback tone when entering this mode
    void (*enter)(void);
    void (*exit)(void);       // may be NULL
    void (*on_tap)(int x, int y); // may be NULL; x/y is where the finger landed
    void (*on_swipe)(int dx, int dy); // horizontal swipes, may be NULL
    void (*on_long_press)(void);
    void (*on_wake)(void);
    int64_t (*next_wake_us)(int64_t now_us);
};

static const ui_mode_t clock_mode;
static const ui_mode_t slideshow_mode;
static const ui_mode_t combined_mode;
#if WEATHER_ENABLE
static const ui_mode_t weather_mode;
#endif
#if RADIO_ENABLE
static const ui_mode_t radio_mode;
#endif

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

static void on_touch_tap(int x, int y)
{
    last_interaction = esp_timer_get_time() / 1000000LL;
    display_backlight_set_brightness(settings.backlight_full_pct);
    last_brightness = settings.backlight_full_pct;
    // While the alarm rings a tap only stops it: the mode keeps its state and
    // redraws itself on the next pass. No tone here, the main task is playing
    // the alarm beeps on the same codec.
    if (alarm_is_ringing()) {
        alarm_dismiss();
        ui_force_redraw();
        return;
    }
    if (active_mode && active_mode->on_tap) {
        active_mode->on_tap(x, y);
    }
    wake_main_task();
}

// Volume is the one gesture that works in every mode, so it lives in the mode
// manager rather than in a mode. The touch task only counts steps; the main
// loop applies them, draws the overlay and stores the new value once per
// gesture.
static volatile int volume_steps = 0;

static void on_swipe(int dx, int dy, int duration_ms)
{
    (void)duration_ms;
    last_interaction = esp_timer_get_time() / 1000000LL;
    display_backlight_set_brightness(settings.backlight_full_pct);
    last_brightness = settings.backlight_full_pct;
    // While it rings the alarm enforces its own minimum volume, so a quieter
    // setting would only make the bar lie about what is heard.
    if (alarm_is_ringing()) return;
    // Horizontal swipes belong to the mode (the radio changes station with
    // them); the volume takes the vertical ones.
    if (abs(dx) > abs(dy) || dy == 0) {
        if (active_mode && active_mode->on_swipe) {
            active_mode->on_swipe(dx, dy);
            wake_main_task();
        }
        return;
    }
    // finger up is louder. The travel decides how many steps one gesture is
    // worth: a careful slide should not be wasted on a single step, and every
    // extra gesture is one more chance to hit something else by accident.
    int steps = abs(dy) / VOLUME_STEP_PX;
    if (steps < 1) steps = 1;
    if (steps > VOLUME_MAX_STEPS) steps = VOLUME_MAX_STEPS;
    __atomic_fetch_add(&volume_steps, dy < 0 ? steps : -steps,
                       __ATOMIC_SEQ_CST);
    wake_main_task();
}

static void apply_volume_steps(void)
{
    int steps = __atomic_exchange_n(&volume_steps, 0, __ATOMIC_SEQ_CST);
    if (steps == 0) return;

    int volume = settings.audio_volume + steps * VOLUME_STEP_PCT;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    audio_set_volume(volume);
    osd_show_volume(volume);
    if (volume != settings.audio_volume) {
        settings.audio_volume = (uint8_t)volume;
        // Once per gesture, not per step: NVS is flash.
        settings_save();
    }
    ESP_LOGI(TAG, "volume %d%%", volume);
}

// Volume steps on behalf of a mode: the radio draws its own buttons and calls
// this, everything else (clamping, overlay, one NVS write per gesture) stays
// here where the gesture also ends up.
void slideshow_volume_steps(int steps)
{
    if (steps == 0) return;
    __atomic_fetch_add(&volume_steps, steps, __ATOMIC_SEQ_CST);
    wake_main_task();
}

static void on_long_press(void)
{
    // Same rule as a tap: the ringing alarm owns the touch screen.
    if (alarm_is_ringing()) {
        last_interaction = esp_timer_get_time() / 1000000LL;
        display_backlight_set_brightness(settings.backlight_full_pct);
        last_brightness = settings.backlight_full_pct;
        alarm_dismiss();
        ui_force_redraw();
        wake_main_task();
        return;
    }
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

// Hand the CPU clock back to the power manager while an image mode idles. The
// clock mode never needs the lock; with SLIDE_PM_LOW_POWER the slide modes do
// not need it outside the image work either.
static void apply_power_for_mode(bool clock_active)
{
#if SLIDE_PM_LOW_POWER
    (void)clock_active;
    // The SD card path takes no power management lock of its own, so a light
    // sleep in the middle of a transfer would corrupt it: image work holds the
    // lock itself, see slide_work_hold().
    cpu_max_hold(false);
#else
    cpu_max_hold(!clock_active);
#endif
}

//
// Mode: clock only (cycle position 1)
//

static void clock_enter(void)
{
    apply_power_for_mode(true);
    combined_overlay = false;
    last_clock_min = -1;
    clock_render(NULL); // black background, draws immediately
    struct tm tm;
    time_t now = time(NULL);
    localtime_r(&now, &tm);
    last_clock_min = tm.tm_min;
}

static void clock_on_tap(int x, int y)
{
    // Nothing to advance in clock mode; the wrapper still resets dimming.
    (void)x;
    (void)y;
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
    combined_overlay = false;
    // Redraw unconditionally: the clock mode may have blanked the shared
    // frame buffer, so the buffered slide cannot be trusted anymore.
    slide_active = false;
    slideshow_step(); // draws the current (or next due) slide right away
}

static void slideshow_on_tap(int x, int y)
{
    (void)x;
    (void)y;
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
    // Every slide from now on is composited with the clock before it is
    // pushed, on entry as well as during a crossfade, so the overlay never
    // fades away with the image.
    combined_overlay = true;
    slideshow_step();
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
    if (redrawn) {
        // show_slide_now() has already composited and pushed the clock.
        last_clock_min = tm.tm_min;
        return;
    }
    if (tm.tm_min == last_clock_min) return;
    // The overlay painted over the slide pixels in the shared buffer, so put
    // the clock-free copy back before drawing the new time. Without that copy
    // (no PSRAM) the slide has to be decoded again, as before.
    if (clean_valid) {
        memcpy(slide_fb, clean_buf, (size_t)shown_w * shown_h * sizeof(uint16_t));
    } else if (slide_count > 0) {
        // show_slide_now() draws the clock into the new frame as well.
        show_slide(slide_paths[slide_idx]);
        last_clock_min = tm.tm_min;
        return;
    }
    clock_render(slide_fb);
    last_clock_min = tm.tm_min;
}

static int64_t combined_next_wake_us(int64_t now_us)
{
    int64_t wake_us = slideshow_next_wake_us(now_us);
    int64_t to_min_us = to_minute_us();
    if (to_min_us < wake_us) wake_us = to_min_us;
    return wake_us;
}

//
// Mode: weather (cycle position 4, only with WEATHER_ENABLE)
//
// The weather module owns the request, the cache and the rendering; the mode
// only hands it the frame time and makes sure the backlight schedule keeps
// running. A tap flips between current conditions and the forecast, so the
// long press stays free for the cycle.

#if WEATHER_ENABLE
static void weather_mode_on_tap(int x, int y)
{
    // The weather pages are flipped by any tap, wherever it lands.
    (void)x;
    (void)y;
    weather_on_tap();
}

static void weather_mode_enter(void)
{
    apply_power_for_mode(true);
    combined_overlay = false;
    weather_enter();
}

static void weather_mode_on_wake(void)
{
    if (take_force_redraw()) weather_redraw();
    weather_on_wake();
}

static int64_t weather_mode_next_wake_us(int64_t now_us)
{
    int64_t wake_us = weather_next_refresh_us();
    int64_t to_dim_us = next_backlight_step_us(now_us);
    if (to_dim_us < wake_us) wake_us = to_dim_us;
    return wake_us;
}
#endif // WEATHER_ENABLE

//
// Mode: radio (cycle position 5, only with RADIO_ENABLE)
//
// The radio module owns the station list, the stream and the screen; the mode
// only hands it the frame time and keeps the backlight schedule running. A tap
// plays and pauses, a horizontal swipe changes station, the long press leaves
// the mode.

#if RADIO_ENABLE
static void radio_mode_enter(void)
{
    apply_power_for_mode(true);
    combined_overlay = false;
    radio_enter();
}

static void radio_mode_on_wake(void)
{
    if (take_force_redraw()) radio_redraw();
    radio_on_wake();
}

static int64_t radio_mode_next_wake_us(int64_t now_us)
{
    int64_t wake_us = radio_next_wake_us(now_us);
    int64_t to_dim_us = next_backlight_step_us(now_us);
    if (to_dim_us < wake_us) wake_us = to_dim_us;
    return wake_us;
}
#endif // RADIO_ENABLE

//
// Mode instances. Long-press cycle: clock -> slideshow -> combined -> weather
// -> clock; the weather step is compiled in only with WEATHER_ENABLE. The
// device boots into clock mode.
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
    .exit = release_slide_resources,
    .on_tap = slideshow_on_tap,
    .on_long_press = mode_on_long_press,
    .on_wake = slideshow_on_wake,
    .next_wake_us = slideshow_next_wake_us,
};

static const ui_mode_t combined_mode = {
    .name = "clock+slideshow",
#if WEATHER_ENABLE
    .next = &weather_mode,
#elif RADIO_ENABLE
    .next = &radio_mode,
#else
    .next = &clock_mode,
#endif
    .enter_tone_hz = 988,
    .enter = combined_enter,
    .exit = release_slide_resources,
    .on_tap = slideshow_on_tap,
    .on_long_press = mode_on_long_press,
    .on_wake = combined_on_wake,
    .next_wake_us = combined_next_wake_us,
};

#if WEATHER_ENABLE
static const ui_mode_t weather_mode = {
    .name = "weather",
#if RADIO_ENABLE
    .next = &radio_mode,
#else
    .next = &clock_mode,
#endif
    .enter_tone_hz = 740,
    .enter = weather_mode_enter,
    .exit = NULL,
    .on_tap = weather_mode_on_tap,
    .on_long_press = mode_on_long_press,
    .on_wake = weather_mode_on_wake,
    .next_wake_us = weather_mode_next_wake_us,
};
#endif // WEATHER_ENABLE

#if RADIO_ENABLE
static const ui_mode_t radio_mode = {
    .name = "radio",
    .next = &clock_mode,
    .enter_tone_hz = 660,
    .enter = radio_mode_enter,
    .exit = radio_exit,
    .on_tap = radio_on_tap,
    .on_swipe = radio_on_swipe,
    .on_long_press = mode_on_long_press,
    .on_wake = radio_mode_on_wake,
    .next_wake_us = radio_mode_next_wake_us,
};
#endif // RADIO_ENABLE

// Short file name for the debug frame dump of a mode: the display names are
// friendly but "clock+slideshow" does not fit into the 8.3 name of the card.
static const char *mode_shot_name(const ui_mode_t *mode)
{
    if (mode == &clock_mode) return "clock";
    if (mode == &slideshow_mode) return "slides";
    if (mode == &combined_mode) return "combined";
#if WEATHER_ENABLE
    if (mode == &weather_mode) return "weather";
#endif
#if RADIO_ENABLE
    if (mode == &radio_mode) return "radio";
#endif
    return "mode";
}

//
// Hold automatic light sleep off while a USB host is connected: the ESP32-S3
// USB Serial/JTAG peripheral cannot survive light sleep, the device would drop
// off the bus and flashing or monitoring would break mid-session.
static void usb_pm_update(void)
{
#if PM_LIGHT_SLEEP
    if (!usb_pm_lock) return;
    bool connected = usb_serial_jtag_is_connected();
    if (connected == usb_pm_hold) return;
    if (connected) {
        esp_pm_lock_acquire(usb_pm_lock);
    } else {
        esp_pm_lock_release(usb_pm_lock);
    }
    usb_pm_hold = connected;
    ESP_LOGI(TAG, "USB host %s, light sleep %s",
             connected ? "connected" : "gone",
             connected ? "blocked" : "allowed");
#else
    (void)usb_pm_hold;
    (void)usb_pm_lock;
#endif
}

// Mode manager main loop.
//

void slideshow_run(void)
{
    slide_fb = display_get_framebuffer();
    clock_init();
#if WEATHER_ENABLE
    weather_init();
#endif
    osd_init();
    last_interaction = esp_timer_get_time() / 1000000LL;

    // The clock ceiling and the idle floor come from power_init(); this lock
    // only keeps the CPU up while image work is in flight.
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "slideshow", &cpu_max_lock);

#if PM_LIGHT_SLEEP
    // Automatic light sleep disables all GPIO pads during sleep. Two pads have
    // to stay alive: the touch interrupt is the wake-up source, and the panel's
    // chip select must stay high so a floating clock line cannot shift noise
    // into the display. The backlight pad is handled by the LEDC driver.
    gpio_sleep_sel_dis((gpio_num_t)PIN_TP_INT);
    gpio_sleep_sel_dis((gpio_num_t)PIN_LCD_CS);
    // The USB Serial/JTAG peripheral dies in light sleep and takes the port
    // with it. Keep a lock while a host is connected; unplugged the chip may
    // sleep as deep as it likes.
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb", &usb_pm_lock);
    usb_pm_update();
#endif

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
    touch_set_swipe_cb(on_swipe);
    active_mode->enter();
    screenshot_capture(mode_shot_name(active_mode));

    // A factory-fresh box has no WiFi credentials and therefore no network to
    // reach its setup page from. It opens its own access point on the first
    // boot and serves the page there; the 5 s gesture still opens it later.
    if (!net_wifi_configured()) web_start();

    while (1) {
        // Perform a pending mode switch before any other work, so the new
        // mode's enter() redraw makes the change visible immediately.
        if (pending_mode) {
            const ui_mode_t *next = pending_mode;
            pending_mode = NULL;
            if (active_mode->exit) active_mode->exit();
            active_mode = next;
            active_mode->enter();
            screenshot_capture(mode_shot_name(active_mode));
        }

#if CLOCK_DEEP_SLEEP
        // Clock mode only, and only inside the sleep window: once the
        // backlight schedule has run down, deep sleep until the window ends
        // (or a touch wakes the board). The check runs before on_wake() and
        // update_backlight(), so a wake-up that finds the window still active
        // stays completely dark - no redraw, no backlight. The touch line must
        // be idle-high, otherwise the ext0 wake would fire right away and
        // loop the boot.
        //
        // A timer wake-up ends that deep sleep, and the backlight then starts
        // its normal schedule, so the clock is readable again at the start of
        // the day: display_init() left the panel dark until this point, and a
        // wake-up that still finds the window active goes straight back to
        // sleep before update_backlight() runs, so nothing flashes at night
        // either.
        if (settings.clock_deep_sleep_enable && active_mode == &clock_mode &&
            clock_deep_sleep_window_active() && !web_is_running() &&
            !alarm_is_ringing() &&
            esp_timer_get_time() / 1000000LL - last_interaction >=
                settings.backlight_deep_after_sec &&
            gpio_get_level((gpio_num_t)PIN_TP_INT) != 0) {
            int64_t sleep_us = clock_deep_sleep_window_end_us();
            // A pending alarm must not be slept over: inside the window the
            // box wakes at the alarm time instead of at the end of the
            // window, rings and goes back to sleep on its next idle pass.
            int64_t to_alarm_us = alarm_next_wake_us(esp_timer_get_time());
            if (to_alarm_us < sleep_us) sleep_us = to_alarm_us;
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

        // The alarm has priority over the mode: it starts on its own and owns
        // the screen while it rings, so the mode does not paint over it.
        alarm_on_wake();
        const bool ringing = alarm_is_ringing();
        if (ringing) {
            // Keep the screen lit while it rings, even after the idle schedule
            // has dimmed it or a deep sleep wake-up left the backlight dark.
            last_interaction = esp_timer_get_time() / 1000000LL;
        } else {
            active_mode->on_wake();
        }
        update_backlight();

        // After the mode has drawn: the overlay has to be the last thing that
        // reaches the panel in this pass, otherwise the mode's redraw would
        // already have replaced it.
        apply_volume_steps();
        osd_on_wake(esp_timer_get_time());

        int64_t now_us = esp_timer_get_time();
        int64_t wake_us = alarm_next_wake_us(now_us);
        if (!ringing) {
            // Not ringing, so this is the time to the next alarm (or
            // INT64_MAX); the mode may want to wake earlier.
            int64_t mode_us = active_mode->next_wake_us(now_us);
            if (mode_us < wake_us) wake_us = mode_us;
        }
        int64_t osd_us = osd_next_wake_us(now_us);
        if (osd_us < wake_us) wake_us = osd_us;
        if (wake_us < 10000) wake_us = 10000;     // avoid a busy spin
        if (wake_us > 60000000) wake_us = 60000000; // cap the safety timeout

        esp_timer_start_once(wake_timer, wake_us);
        usb_pm_update();
        // Woken by the timer or by touch; timeout is a safety net.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wake_us / 1000 + 2000));
        esp_timer_stop(wake_timer);
    }
}
