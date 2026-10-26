#include "radio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "draw.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "pls.h"
#include "sdcard.h"
#include "screenshot.h"
#include "settings.h"
#include "slideshow.h"
#include "status.h"
#include "stream.h"

static const char *TAG = "radio";

#define COL_BG 0x0000
#define COL_TEXT 0xFFFF
#define COL_DIM 0x8C71
#define COL_AMBER 0xFD20
#define COL_ERROR 0xF800

// The play/pause button sits in the middle of the screen: a tap there toggles
// playback, a tap above or below it changes the volume. Taps are the reliable
// input on this panel - a slide the controller only half reports ends as a tap,
// and a station that pauses itself is the one thing this mode must not do by
// accident. So the destructive action gets the small, visible target and the
// harmless one gets the rest of the screen.
#define BTN_CX 120
#define BTN_CY 120
#define BTN_R 24
#define BTN_HIT_R 38
// A tap on either volume half is worth two steps (10 %). Sliding still gives
// one step per 40 px, which is where fine control comes from.
#define ZONE_TAP_STEPS 2

// Retry delays after a failed attempt, in milliseconds. The last one repeats,
// so a station that comes back is picked up without a tap.
#define BACKOFF_COUNT 3
static const int BACKOFF_MS[BACKOFF_COUNT] = {2000, 5000, 10000};

static pls_station_t *stations = NULL;
static int station_count = 0;
static int station_index = 0;
static int mirror = 0;
static bool paused = false;
static int backoff_step = 0;
static int64_t retry_at_us = 0;

// Set by the touch task, applied by the main task. The tap keeps the point it
// landed on, which decides between the button and the volume halves.
static volatile bool tap_pending = false;
static volatile int tap_x = 0;
static volatile int tap_y = 0;
static volatile int swipe_pending = 0;

// What the panel currently shows, so a redraw only happens on a change.
static stream_state_t drawn_state = STREAM_STOPPED;
static int drawn_index = -1;
static bool drawn_paused = false;

static void push(uint16_t *fb)
{
    ui_status_draw(fb);
    esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0, SCREEN_W, SCREEN_H, fb);
}

static const char *state_text(void)
{
    if (paused) return "Paused";
    switch (stream_state()) {
    case STREAM_CONNECTING:
        return "Connecting";
    case STREAM_BUFFERING:
        return "Buffering";
    case STREAM_PLAYING:
        return "Playing";
    case STREAM_FAILED:
        return "No signal";
    default:
        return "Stopped";
    }
}

// Pause bars while something is playing or on its way, an arrow otherwise: a
// tap on the button then always means the obvious thing.
static bool shows_pause_icon(void)
{
    if (paused) return false;
    const stream_state_t s = stream_state();
    return s == STREAM_CONNECTING || s == STREAM_BUFFERING ||
           s == STREAM_PLAYING;
}

static void draw_button(uint16_t *fb)
{
    draw_ring(BTN_CX, BTN_CY, BTN_R, 1, COL_DIM, fb);
    if (shows_pause_icon()) {
        fill_rect(BTN_CX - 8, BTN_CY - 9, 6, 18, COL_TEXT, fb);
        fill_rect(BTN_CX + 2, BTN_CY - 9, 6, 18, COL_TEXT, fb);
    } else {
        // Right-pointing triangle, one column per step of the slope.
        for (int i = 0; i < 13; i++) {
            const int h = 18 - i * 18 / 13;
            const int x = BTN_CX - 6 + i;
            draw_line(x, BTN_CY - h / 2, x, BTN_CY + h / 2, 1, COL_TEXT, fb);
        }
    }
}

static bool in_button(int x, int y)
{
    const int dx = x - BTN_CX;
    const int dy = y - BTN_CY;
    return dx * dx + dy * dy <= BTN_HIT_R * BTN_HIT_R;
}

static void render(void)
{
    uint16_t *fb = display_get_framebuffer();
    fill_rect(0, 0, SCREEN_W, SCREEN_H, COL_BG, fb);

    if (station_count == 0) {
        draw_text_center("No stations", 100, 0, COL_TEXT, fb);
        draw_text_center("Add .pls files", 126, 0, COL_DIM, fb);
        draw_text_center("to the radio folder", 148, 0, COL_DIM, fb);
        push(fb);
        return;
    }

    const pls_station_t *st = &stations[station_index];
    draw_text_center("VOL +", 8, 0, COL_DIM, fb);
    draw_text_center_fit(st->name, 54, 200, COL_TEXT, fb);
    if (st->title[0]) draw_text_center_fit(st->title, 78, 196, COL_DIM, fb);
    draw_button(fb);

    const stream_state_t s = stream_state();
    uint16_t color = COL_TEXT;
    if (s == STREAM_FAILED) {
        color = COL_ERROR;
    } else if (s == STREAM_CONNECTING || s == STREAM_BUFFERING) {
        color = COL_AMBER;
    }
    draw_text_center(state_text(), 152, 0, color, fb);

    if (stream_rate_hz() > 0) {
        char meta[40];
        snprintf(meta, sizeof(meta), "MP3 %d kbit/s  %d.%d kHz", stream_kbps(),
                 stream_rate_hz() / 1000, (stream_rate_hz() % 1000) / 100);
        draw_text_center(meta, 174, 0, COL_DIM, fb);
    } else if (s == STREAM_FAILED && stream_error()[0]) {
        draw_text_center_fit(stream_error(), 174, 200, COL_DIM, fb);
    }

    char pos[16];
    snprintf(pos, sizeof(pos), "%d / %d", station_index + 1, station_count);
    draw_text_center(pos, 196, 0, COL_DIM, fb);
    draw_text_center("VOL -", 214, 0, COL_DIM, fb);

    push(fb);
}

// Scans the card. Keeps the previous list when the card or the folder is
// missing, so a mode switch does not blank a working station list.
static void scan_stations(void)
{
    if (!stations) {
        stations = heap_caps_malloc(sizeof(pls_station_t) * PLS_MAX_STATIONS,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!stations) {
            ESP_LOGE(TAG, "no memory for the station list");
            return;
        }
    }
    if (!sdcard_is_mounted() && sdcard_init() != ESP_OK) {
        station_count = 0;
        return;
    }
    char dir[64];
    snprintf(dir, sizeof(dir), "/sdcard%s", settings.radio_dir);
    station_count = pls_scan(dir, stations, PLS_MAX_STATIONS);
    if (station_index >= station_count) station_index = 0;
}

static void begin_stream(void)
{
    if (station_count == 0) return;
    const pls_station_t *st = &stations[station_index];
    if (st->url_count == 0) return;
    if (mirror >= st->url_count) mirror = 0;
    paused = false;
    retry_at_us = 0;
    ESP_LOGI(TAG, "station %d/%d '%s', mirror %d/%d", station_index + 1,
             station_count, st->name, mirror + 1, st->url_count);
    if (stream_start(st->urls[mirror]) != ESP_OK) {
        stream_stop();
        // Too little memory for the tasks: do not hammer the allocator, the
        // next wake tries again after a pause.
        retry_at_us = esp_timer_get_time() + 5000000;
    }
}

// Next address and delay after a failed attempt. With several mirrors the
// rotation ends up trying each of them before it settles into the last delay.
static void schedule_retry(void)
{
    const uint8_t n = stations[station_index].url_count;
    if (n > 1) mirror = (uint8_t)((mirror + 1) % n);
    const int ms = BACKOFF_MS[backoff_step < BACKOFF_COUNT ? backoff_step
                                                          : BACKOFF_COUNT - 1];
    if (backoff_step < BACKOFF_COUNT) backoff_step++;
    retry_at_us = esp_timer_get_time() + (int64_t)ms * 1000;
    ESP_LOGW(TAG, "retry in %d ms", ms);
}

static void change_station(int step)
{
    if (station_count == 0) return;
    station_index = (station_index + step + station_count) % station_count;
    mirror = 0;
    backoff_step = 0;
    paused = false;
    stream_stop();
    begin_stream();
}

void radio_enter(void)
{
    tap_pending = false;
    swipe_pending = 0;
    backoff_step = 0;
    retry_at_us = 0;
    scan_stations();
    if (station_count > 0) begin_stream();
    render();
    drawn_state = stream_state();
    drawn_index = station_index;
    drawn_paused = paused;
    screenshot_capture("radio");
}

void radio_redraw(void)
{
    render();
    drawn_state = stream_state();
    drawn_index = station_index;
    drawn_paused = paused;
}

void radio_exit(void)
{
    stream_stop();
    paused = false;
    station_count = 0;
    drawn_index = -1;
    if (stations) {
        heap_caps_free(stations);
        stations = NULL;
    }
}

void radio_on_tap(int x, int y)
{
    // Read by the main task together with the flag, so the point cannot arrive
    // after the tap it belongs to.
    tap_x = x;
    tap_y = y;
    tap_pending = true;
}

void radio_on_swipe(int dx, int dy)
{
    (void)dy;
    __atomic_fetch_add(&swipe_pending, dx < 0 ? 1 : -1, __ATOMIC_SEQ_CST);
}

void radio_on_wake(void)
{
    bool redraw = false;

    const int swipe = __atomic_exchange_n(&swipe_pending, 0, __ATOMIC_SEQ_CST);
    if (swipe != 0) {
        change_station(swipe > 0 ? 1 : -1);
        redraw = true;
    }

    if (tap_pending) {
        tap_pending = false;
        if (station_count == 0) {
            // Nothing to play yet: any tap asks for a fresh look at the card.
            scan_stations();
            if (station_count > 0) begin_stream();
        } else if (in_button(tap_x, tap_y)) {
            const stream_state_t s = stream_state();
            if (paused) {
                begin_stream();
            } else if (s == STREAM_FAILED) {
                backoff_step = 0;
                begin_stream();
            } else {
                stream_stop();
                paused = true;
            }
        } else {
            // Everything above the button is louder, everything below it
            // quieter. A tap that was meant as a slide does something harmless
            // here instead of stopping the station.
            slideshow_volume_steps(tap_y < BTN_CY ? ZONE_TAP_STEPS
                                                  : -ZONE_TAP_STEPS);
        }
        redraw = true;
    }

    // A failed stream retries on its own: a radio that comes back when the
    // network does is better than one that waits for a tap.
    if (!paused && station_count > 0) {
        const stream_state_t now = stream_state();
        if (now == STREAM_FAILED) {
            if (retry_at_us == 0) {
                schedule_retry();
                redraw = true;
            } else if (esp_timer_get_time() >= retry_at_us) {
                begin_stream();
                redraw = true;
            }
        } else if (now == STREAM_STOPPED && esp_timer_get_time() >= retry_at_us) {
            begin_stream();
            redraw = true;
        }
    }

    const stream_state_t s = stream_state();
    if (s != drawn_state || station_index != drawn_index ||
        paused != drawn_paused) {
        redraw = true;
    }
    if (redraw) {
        render();
        drawn_state = s;
        drawn_index = station_index;
        drawn_paused = paused;
    }
}

int64_t radio_next_wake_us(int64_t now_us)
{
    if (station_count == 0 || paused) return 60000000LL; // nothing to poll
    const stream_state_t s = stream_state();
    if (s == STREAM_FAILED || s == STREAM_STOPPED) {
        const int64_t left = retry_at_us - now_us;
        return left > 0 ? left : 0;
    }
    if (s == STREAM_PLAYING) return 2000000; // notice a stream that dies
    return 200000; // connecting or buffering: the screen changes soon
}
