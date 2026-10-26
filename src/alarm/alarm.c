#include "alarm.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "audio.h"
#include "clock.h"
#include "display.h"
#include "draw.h"
#include "settings.h"

static const char *TAG = "alarm";

// A daily alarm: at the configured local time the speaker beeps and the screen
// shows the clock with an "ALARM" label, whatever mode is active. A tap (or a
// long press) stops it, and it stops by itself after ALARM_AUTO_STOP_SEC so a
// forgotten box does not beep until the battery is empty.
//
// The module is called from the main loop of slideshow.c, which also makes sure
// the night deep sleep does not skip the alarm.
#define ALARM_GRACE_SEC 60     // how late the loop may notice and still ring
#define ALARM_TICK_US 1000000LL
#define ALARM_BEEP_PERIOD_US 2000000LL
#define ALARM_AUTO_STOP_SEC 300
// The alarm must be heard even when the feedback tones are turned down.
#define ALARM_MIN_VOLUME_PCT 70

#define COLOR_ALARM 0xF800 // red, the label and its blink
#define COLOR_HINT 0x8410  // grey, the "tap to stop" row

// `ringing` is written by the touch task (dismiss) and read by the main loop,
// so it has to be volatile.
static volatile bool ringing = false;
static int64_t ring_started_us = 0;
static int64_t next_beep_us = 0;
static time_t last_rang = 0; // wall clock when the last ring started

// Local wall clock of the configured alarm time on the day of ref, or on the
// following day with add_days = 1. mktime() normalizes the date arithmetic and
// applies the summer time rule of the configured zone.
static time_t alarm_time_on(time_t ref, int add_days)
{
    struct tm tm;
    localtime_r(&ref, &tm);
    tm.tm_mday += add_days;
    tm.tm_hour = settings.alarm_hour;
    tm.tm_min = settings.alarm_minute;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

// True when both timestamps fall on the same local calendar day; the alarm
// must not ring twice before midnight.
static bool same_day(time_t a, time_t b)
{
    struct tm ta, tb;
    localtime_r(&a, &ta);
    localtime_r(&b, &tb);
    return ta.tm_year == tb.tm_year && ta.tm_yday == tb.tm_yday;
}

// The alarm screen: the normal clock face with a blinking label above it and
// the hint below it. clock_compose() draws into the shared frame buffer without
// pushing it, so the whole frame leaves in one transfer.
static void render(void)
{
    if (!ringing) return;

    uint16_t *fb = display_get_framebuffer();
    memset(fb, 0, 240 * 240 * sizeof(uint16_t)); // the mode content is gone
    clock_compose(fb);

    bool on = ((esp_timer_get_time() - ring_started_us) / 500000LL) & 1;
    if (on) draw_text_center("ALARM", 20, 0, COLOR_ALARM, fb);
    draw_text_center("tap to stop", 196, 0, COLOR_HINT, fb);
    esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0, 240, 240, fb);
}

static void start_ringing(void)
{
    last_rang = time(NULL);
    ringing = true;
    ring_started_us = esp_timer_get_time();
    next_beep_us = ring_started_us; // the first pair of beeps is due right now
    ESP_LOGI(TAG, "alarm at %02d:%02d", settings.alarm_hour, settings.alarm_minute);
}

bool alarm_is_ringing(void)
{
    return ringing;
}

void alarm_dismiss(void)
{
    if (!ringing) return;
    ringing = false;
    ESP_LOGI(TAG, "alarm stopped");
}

void alarm_on_wake(void)
{
    if (!ringing) {
        if (!settings.alarm_enabled) return;
        time_t now = time(NULL);
        time_t due = alarm_time_on(now, 0);
        if (now < due || now - due >= ALARM_GRACE_SEC) return;
        if (last_rang != 0 && same_day(now, last_rang)) return;
        start_ringing();
    }

    const int64_t now_us = esp_timer_get_time();
    if (now_us - ring_started_us >= (int64_t)ALARM_AUTO_STOP_SEC * 1000000LL) {
        ESP_LOGI(TAG, "alarm stopped after %d s", ALARM_AUTO_STOP_SEC);
        alarm_dismiss();
        return;
    }
    if (now_us >= next_beep_us) {
        // Two rising tones per period. The call blocks for the length of the
        // beep, which the main loop can afford; a tap still arrives through
        // the touch task meanwhile. The volume floor keeps a box whose
        // feedback volume is near zero from missing its own alarm.
        int volume = settings.audio_volume < ALARM_MIN_VOLUME_PCT
                         ? ALARM_MIN_VOLUME_PCT
                         : settings.audio_volume;
        audio_play_tone_at(1046, 140, volume);
        audio_play_tone_at(1318, 180, volume);
        next_beep_us = now_us + ALARM_BEEP_PERIOD_US;
    }
    render();
}

int64_t alarm_next_wake_us(int64_t now_us)
{
    (void)now_us;
    if (ringing) return ALARM_TICK_US;
    if (!settings.alarm_enabled) return INT64_MAX;

    time_t now = time(NULL);
    time_t due = alarm_time_on(now, 0);
    if (now >= due && now - due < ALARM_GRACE_SEC &&
        !(last_rang != 0 && same_day(now, last_rang))) {
        return 0; // due in this pass, alarm_on_wake() picks it up
    }
    if (now < due) return (int64_t)(due - now) * 1000000LL;
    // The time has passed (later that day, or the alarm already rang): the
    // next chance is tomorrow at the same local time.
    return (int64_t)(alarm_time_on(now, 1) - now) * 1000000LL;
}
