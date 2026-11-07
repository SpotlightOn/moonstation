#include "movie.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "display.h"
#include "draw.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "player.h"
#include "sdcard.h"
#include "screenshot.h"
#include "settings.h"
#include "status.h"

static const char *TAG = "movie_ui";

#define MAX_MOVIES 64
// Waiting for the frame that is in flight when a pause lands: the playback
// task may still be converting or pushing one, and drawing the pause band
// before it would only make it disappear again.
#define PAUSE_SETTLE_MS 150

#define COL_BG 0x0000
#define COL_TEXT 0xFFFF
#define COL_DIM 0x8C71
#define COL_ERROR 0xF800
#define COL_WARN 0xFFE0

typedef enum {
    ST_EMPTY,   // nothing to play (no card, no files)
    ST_PLAYING,
    ST_PAUSED,
    ST_ERROR,   // the current file could not be played
} state_t;

static char *paths[MAX_MOVIES];
static size_t count = 0;
static size_t movie_index = 0;
static state_t state = ST_EMPTY;

// Set by the touch task, applied by the main task.
static volatile bool tap_pending = false;
static volatile int swipe_pending = 0;

// Text of the last hint, kept so a redraw (status row, force redraw) can put
// the same screen back without knowing why it is there.
static char hint_top[32];
static char hint_bottom[64];
static uint16_t hint_color = COL_TEXT;

static void push(uint16_t *fb)
{
    ui_status_draw(fb);
    display_draw_bitmap(0, 0, SCREEN_W, SCREEN_H, fb);
}

static void render_hint(void)
{
    uint16_t *fb = display_get_framebuffer();
    fill_rect(0, 0, SCREEN_W, SCREEN_H, COL_BG, fb);
    draw_text_center(hint_top, 96, 0, hint_color, fb);
    if (hint_bottom[0]) draw_text_center_fit(hint_bottom, 122, 216, COL_DIM, fb);
    push(fb);
}

static void show_hint(const char *top, const char *bottom, uint16_t color)
{
    snprintf(hint_top, sizeof(hint_top), "%s", top);
    snprintf(hint_bottom, sizeof(hint_bottom), "%s", bottom ? bottom : "");
    hint_color = color;
    render_hint();
}

// File name without its directory, for the pause overlay and the error line.
static const char *file_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void base_name(const char *path, char *out, size_t cap)
{
    snprintf(out, cap, "%s", file_name(path));
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = '\0';
}

static void dim_band(uint16_t *fb, int y0, int h)
{
    for (int y = y0; y < y0 + h; y++) {
        uint16_t *row = fb + (size_t)y * SCREEN_W;
        for (int x = 0; x < SCREEN_W; x++) {
            const uint16_t v = RGB565_SWAP(row[x]);
            const unsigned r = (v >> 11) & 0x1F;
            const unsigned g = (v >> 5) & 0x3F;
            const unsigned b = v & 0x1F;
            row[x] = RGB565_SWAP((uint16_t)(((r >> 1) << 11) | ((g >> 1) << 5) |
                                            (b >> 1)));
        }
    }
}

// The pause overlay goes into the frame the player keeps, so it covers the
// picture that is actually on the panel instead of a black buffer.
static void show_paused(void)
{
    uint16_t *fb = movie_player_framebuffer();
    if (!fb) return;
    dim_band(fb, 88, 64);
    draw_line(24, 88, 215, 88, 1, COL_DIM, fb);
    draw_line(24, 151, 215, 151, 1, COL_DIM, fb);
    draw_text_center("Paused", 96, 0, COL_TEXT, fb);
    char name[64];
    if (count > 0) base_name(paths[movie_index], name, sizeof(name));
    else name[0] = '\0';
    draw_text_center_fit(name, 120, 200, COL_DIM, fb);
    push(fb);
}

static bool is_movie(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    return strcasecmp(ext, ".ts") == 0;
}

static int cmp_str(const void *a, const void *b)
{
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcmp(sa, sb);
}

static void free_movies(void)
{
    for (size_t i = 0; i < count; i++) {
        free(paths[i]);
        paths[i] = NULL;
    }
    count = 0;
}

static void scan_movies(void)
{
    free_movies();
    if (!sdcard_is_mounted() && sdcard_init() != ESP_OK) {
        ESP_LOGW(TAG, "no SD card");
        return;
    }
    char dir[64];
    snprintf(dir, sizeof(dir), "/sdcard%s", settings.movie_dir);
    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGW(TAG, "cannot open %s", dir);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && count < MAX_MOVIES) {
        if (!is_movie(e->d_name)) continue;
        const size_t plen = strlen(dir) + 1 + strlen(e->d_name) + 1;
        char *p = malloc(plen);
        if (!p) continue;
        snprintf(p, plen, "%s/%s", dir, e->d_name);
        paths[count++] = p;
    }
    closedir(d);
    qsort(paths, count, sizeof(char *), cmp_str);
    ESP_LOGI(TAG, "found %u movies in %s", (unsigned)count, dir);
}

static void start_current(void)
{
    if (count == 0) {
        char bottom[64];
        snprintf(bottom, sizeof(bottom), "Add .ts files to %s",
                 settings.movie_dir);
        show_hint("No movies", bottom, COL_WARN);
        state = ST_EMPTY;
        return;
    }
    const esp_err_t err = movie_player_start(paths[movie_index]);
    if (err != ESP_OK) {
        char name[64];
        base_name(paths[movie_index], name, sizeof(name));
        show_hint("Cannot play", name, COL_ERROR);
        state = ST_ERROR;
        return;
    }
    state = ST_PLAYING;
    ESP_LOGI(TAG, "movie %u/%u: %s", (unsigned)(movie_index + 1), (unsigned)count,
             paths[movie_index]);
}

void movie_enter(void)
{
    tap_pending = false;
    swipe_pending = 0;
    state = ST_EMPTY;
    scan_movies();
    if (count == 0) {
        char bottom[64];
        snprintf(bottom, sizeof(bottom), "Add .ts files to %s",
                 settings.movie_dir);
        show_hint("No movies", bottom, COL_WARN);
    } else {
        start_current();
    }
    screenshot_capture("movie");
}

void movie_exit(void)
{
    movie_player_stop();
    free_movies();
    movie_index = 0;
    state = ST_EMPTY;
}

void movie_redraw(void)
{
    if (state == ST_PAUSED) {
        show_paused();
    } else if (state == ST_EMPTY || state == ST_ERROR) {
        render_hint();
    }
}

void movie_on_tap(int x, int y)
{
    (void)x;
    (void)y;
    tap_pending = true;
}

void movie_on_swipe(int dx, int dy)
{
    (void)dy;
    // Same direction rule as the radio: a swipe to the left moves forward.
    __atomic_fetch_add(&swipe_pending, dx < 0 ? 1 : -1, __ATOMIC_SEQ_CST);
}

static void toggle_pause(void)
{
    if (state == ST_PLAYING) {
        movie_player_set_paused(true);
        state = ST_PAUSED;
        // Let a frame that is already in flight reach the panel before the
        // band goes over it.
        vTaskDelay(pdMS_TO_TICKS(PAUSE_SETTLE_MS));
        show_paused();
    } else if (state == ST_PAUSED) {
        movie_player_set_paused(false);
        state = ST_PLAYING;
        // The next picture lands within a frame interval and takes the band
        // with it; nothing to draw here.
    }
}

void movie_on_wake(void)
{
    const int swipe = __atomic_exchange_n(&swipe_pending, 0, __ATOMIC_SEQ_CST);
    if (swipe != 0 && count > 0) {
        movie_index = (movie_index + (swipe > 0 ? 1 : -1) + count) % count;
        start_current();
        return;
    }

    if (tap_pending) {
        tap_pending = false;
        if (state == ST_PLAYING || state == ST_PAUSED) {
            toggle_pause();
        } else {
            // Nothing playing: any tap asks for a fresh look at the card.
            scan_movies();
            start_current();
        }
        return;
    }

    if (state == ST_PLAYING && movie_player_finished()) {
        const bool failed = movie_player_failed();
        movie_player_stop();
        if (failed) {
            char name[64];
            base_name(paths[movie_index], name, sizeof(name));
            show_hint("Cannot play", name, COL_ERROR);
            state = ST_ERROR;
        } else {
            start_current(); // loop the same movie
        }
    }
}

int64_t movie_next_wake_us(int64_t now_us)
{
    (void)now_us;
    // The picture has its own task and a finish asks for a redraw, so the
    // mode itself has nothing to poll. The manager folds in the backlight
    // steps; this is only the safety net.
    return 60000000LL;
}
