#include "clock.h"
#include "battery.h"
#include "display.h"
#include "draw.h"
#include "status.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "stdbool.h"
#include "stdlib.h"
#include "sys/time.h"
#include "time.h"
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "features.h"
#include "settings.h"

// The radio and the SNTP stack live in core/net.c: the setup web server
// needs the same bring-up, and two copies of it would drift apart.
#include "net.h"

// Point localtime() at the configured zone via a POSIX TZ string. The EU
// rules switch at 01:00 UTC, which matches the POSIX default switch times.
void clock_apply_timezone(void)
{
    // POSIX offsets are inverted: local = UTC - offset.
    int offset_min = -settings.tz_std_offset_min;
    char tz[40];
    int h = offset_min / 60;
    int m = abs(offset_min % 60);
    if (m) {
        snprintf(tz, sizeof(tz), "XXX%d:%02d", h, m);
    } else {
        snprintf(tz, sizeof(tz), "XXX%d", h);
    }
    if (settings.tz_eu_dst) {
        strncat(tz, "YYY,M3.5.0/2,M10.5.0/3", sizeof(tz) - strlen(tz) - 1);
    }
    setenv("TZ", tz, 1);
    tzset();
}

// Geometry of the battery icon, in pixels. Kept small so it sits quietly
// below the weekday instead of competing with the date font.
#define BATT_ICON_W 18
#define BATT_ICON_H 10
#define BATT_NOB_W 2
#define BATT_NOB_H 4
#define BATT_FILL_W (BATT_ICON_W - 4)
#define BATT_ARROW_W 5
#define BATT_ARROW_H 7

// Colors are plain RGB565 (0xF800 = red). Every path that writes pixels
// byte swaps them once on the way into the frame buffer, see RGB565_SWAP
// in display.h, so callers always work with natural color values.
#define COLOR_BATT_LOW 0xF800      // red
#define COLOR_BATT_CHARGING 0x01E0 // dark green, so the icon does not glare
#define COLOR_BATT_NORMAL 0xFFFF   // white
#define COLOR_IP 0xAD55             // mid grey, the address must not glare
#define COLOR_WHITE 0xFFFF         // white
#define COLOR_BLACK 0x0000         // black

static const char *TAG = "clock";

// Any system time above this threshold is real wall-clock time: either NTP
// set it, or the RTC kept it across a deep sleep wake-up.
#define CLOCK_TIME_MIN_VALID 1700000000 // 2023-11-14

static void set_compile_time(void)
{
    // The RTC slow clock keeps running through deep sleep while the battery
    // is connected, so after a wake-up the time is still valid and must not
    // be overwritten with the compile time.
    if (time(NULL) > CLOCK_TIME_MIN_VALID) {
        ESP_LOGI(TAG, "RTC kept the time across sleep, keeping it");
        return;
    }

    // Parse __DATE__ "Mmm dd yyyy" and __TIME__ "hh:mm:ss".
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mstr[4];
    int day, year, hour, min, sec;
    sscanf(__DATE__, "%3s %d %d", mstr, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hour, &min, &sec);
    int month = (strstr(months, mstr) - months) / 3;

    struct tm t = {0};
    t.tm_year = year - 1900;
    t.tm_mon = month;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = sec;
    // __TIME__ is local time, so let mktime() work out whether summer time
    // applies. A zero here would commit to standard time and put the clock
    // one hour ahead for the whole summer.
    t.tm_isdst = -1;
    time_t now = mktime(&t);
    struct timeval tv = { .tv_sec = now, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

bool clock_deep_sleep_window_active(void)
{
    if (settings.clock_deep_sleep_from == settings.clock_deep_sleep_to) return false;
    time_t now;
    time(&now);
    struct tm tm;
    localtime_r(&now, &tm);
    if (settings.clock_deep_sleep_from < settings.clock_deep_sleep_to) {
        return tm.tm_hour >= settings.clock_deep_sleep_from && tm.tm_hour < settings.clock_deep_sleep_to;
    }
    // The window crosses midnight (e.g. 23 -> 7).
    return tm.tm_hour >= settings.clock_deep_sleep_from || tm.tm_hour < settings.clock_deep_sleep_to;
}

// Microseconds until the sleep window ends, 0 while the window is inactive.
// The dark sleep during the window lasts until the end (not per minute), so
// the display never flashes at night; touch still wakes the board any time.
int64_t clock_deep_sleep_window_end_us(void)
{
    if (!clock_deep_sleep_window_active()) return 0;
    time_t now;
    time(&now);
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = settings.clock_deep_sleep_to;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t end = mktime(&tm);
    if (end <= now) end += 24 * 60 * 60; // the window crosses midnight
    return (int64_t)(end - now) * 1000000LL;
}

// One sync cycle: radio up, first NTP answer, radio off again. Shared by the
// boot and by the daily resync. The hold keeps the link up while the setup
// server listens, so a sync at night cannot take the page down.
static void ntp_sync_cycle(const char *reason)
{
    net_wifi_hold();
    if (!net_wifi_up()) {
        ESP_LOGW(TAG, "no WiFi for the %s sync, keeping the time from before", reason);
        net_wifi_release();
        return;
    }
    net_ntp_sync();
    net_wifi_release();
    net_wifi_down();
}

static esp_timer_handle_t ntp_timer = NULL;

// Microseconds until the next configured sync hour in local time.
static int64_t ntp_sync_delay_us(void)
{
    time_t now;
    time(&now);
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = settings.ntp_daily_sync_hour;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t next = mktime(&tm);
    if (next <= now) next += 24 * 60 * 60;
    return (int64_t)(next - now) * 1000000LL;
}

// The cycle takes seconds, which the esp_timer task must not spend blocked,
// so it runs in its own task and re-arms the timer afterwards.
static void ntp_sync_timer_task(void *arg)
{
    ntp_sync_cycle("daily");
    esp_timer_start_once(ntp_timer, ntp_sync_delay_us());
    vTaskDelete(NULL);
}

static void ntp_sync_timer_cb(void *arg)
{
    xTaskCreate(ntp_sync_timer_task, "ntp_sync", 4096, NULL, 5, NULL);
}

// Arms the one-shot timer for the next occurrence of the sync hour.
static void ntp_sync_schedule(void)
{
    if (settings.ntp_daily_sync_hour < 0 || !net_wifi_configured()) return;
    const esp_timer_create_args_t timer_args = {
        .callback = ntp_sync_timer_cb,
        .name = "ntp_sync",
    };
    if (esp_timer_create(&timer_args, &ntp_timer) != ESP_OK) {
        ESP_LOGW(TAG, "daily NTP timer unavailable");
        return;
    }
    esp_timer_start_once(ntp_timer, ntp_sync_delay_us());
    ESP_LOGI(TAG, "daily NTP sync armed for %02d:00", settings.ntp_daily_sync_hour);
}

esp_err_t clock_init(void)
{
    clock_apply_timezone(); // before anything interprets wall-clock time
    set_compile_time();
#if CLOCK_SHOW_BATTERY
    if (battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "battery indicator unavailable");
    }
#endif
    bool deep_sleep_wake = esp_sleep_get_wakeup_causes() & (1 << ESP_SLEEP_WAKEUP_TIMER);
    // Deep sleep wake-ups stay radio-free while the sleep window lasts; the
    // first wake outside it syncs via NTP again, so drift cannot pile up.
    if (net_wifi_configured() && !(deep_sleep_wake && clock_deep_sleep_window_active())) {
        ntp_sync_cycle("boot");
    } else if (net_wifi_configured()) {
        ESP_LOGI(TAG, "deep sleep wake-up inside the sleep window, skipping WiFi/NTP");
    } else {
        ESP_LOGI(TAG, "no WiFi configured, clock keeps compile time");
    }
    ntp_sync_schedule();
    ESP_LOGI(TAG, "clock ready");
    return ESP_OK;
}

// Address of the setup page, drawn in the row of the battery indicator: two
// lines, because the round display cuts off the outer characters further
// down and a single line of fourteen digits would not fit there anymore.
static void draw_setup_ip(const char *ip, int y, uint16_t *fb) {
    char line[20];
    int len = (int)strlen(ip);
    // Break after the last dot that leaves both halves short enough.
    int split = 0;
    for (int i = 0; i < len && i < 11; i++) {
        if (ip[i] == '.' && len - (i + 1) <= 11) split = i + 1;
    }
    snprintf(line, sizeof(line), "%.*s", split ? split : len, ip);
    draw_text_center(line, y, 0, COLOR_IP, fb);
    if (split > 0) {
        snprintf(line, sizeof(line), "%s", ip + split);
        draw_text_center(line, y + draw_text_height(), 0, COLOR_IP, fb);
    }
}

#if CLOCK_SHOW_BATTERY
// Upward pointing arrow shown left of the icon while the battery charges:
// a triangular head followed by a short stem, sized from the icon macros.
static void draw_charge_arrow(int x, int y, uint16_t color, uint16_t *fb) {
    int head_h = (BATT_ARROW_W + 1) / 2;
    for (int row = 0; row < head_h; row++) {
        int w = 1 + 2 * row;
        fill_rect(x + (BATT_ARROW_W - w) / 2, y + row, w, 1, color, fb);
    }
    fill_rect(x + BATT_ARROW_W / 2, y + head_h, 1, BATT_ARROW_H - head_h, color, fb);
}

// Battery icon: outlined body with a dark interior, terminal nub on the right
// and a fill bar covering the charge level.
static void draw_battery_icon(int x, int y, int level, uint16_t color, uint16_t *fb) {
    fill_rect(x, y, BATT_ICON_W, BATT_ICON_H, color, fb);
    fill_rect(x + 1, y + 1, BATT_ICON_W - 2, BATT_ICON_H - 2, COLOR_BLACK, fb);
    fill_rect(x + BATT_ICON_W, y + (BATT_ICON_H - BATT_NOB_H) / 2,
              BATT_NOB_W, BATT_NOB_H, color, fb);

    int w = (BATT_FILL_W * level + 50) / 100;
    if (level > 0 && w < 1) w = 1;
    if (w > BATT_FILL_W) w = BATT_FILL_W;
    fill_rect(x + 2, y + 2, w, BATT_ICON_H - 4, color, fb);
}

// Battery row: [charge arrow] icon + percent, centered on the screen.
static void draw_battery_row(int x, int y, uint16_t *fb) {
    int level = battery_level();
    if (level < 0) return; // monitor not available, draw nothing
    bool charging = battery_is_charging();
    if (level > 100) level = 100;

    uint16_t color = COLOR_BATT_NORMAL;
    if (charging) {
        color = COLOR_BATT_CHARGING;
    } else if (level <= settings.battery_warn_pct) {
        color = COLOR_BATT_LOW;
    }

    char pbuf[8];
    snprintf(pbuf, sizeof(pbuf), "%d%%", level);

    int gap = 6;
    int arrow_w = charging ? BATT_ARROW_W + 4 : 0;
    int icon_w = BATT_ICON_W + BATT_NOB_W;
    int total = arrow_w + icon_w + gap + draw_text_width(pbuf);
    int gx = (240 - total) / 2 + x;

    // The icon is shorter than the text, so center it on the text row.
    int iy = y + (draw_text_height() - BATT_ICON_H) / 2;

    if (charging) {
        draw_charge_arrow(gx, iy + (BATT_ICON_H - BATT_ARROW_H) / 2, color, fb);
        gx += arrow_w;
    }
    draw_battery_icon(gx, iy, level, color, fb);
    draw_text(pbuf, gx + icon_w + gap, y, color, fb);
}
#endif // CLOCK_SHOW_BATTERY

void clock_compose(uint16_t *fb)
{
    time_t now;
    time(&now);
    struct tm tm;
    localtime_r(&now, &tm);

    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    int tw = draw_time_width(tbuf);
    draw_time_text(tbuf, (240 - tw) / 2, 70, COLOR_WHITE, fb);

    char dbuf[64];
    static const char *wdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    // Weekday and date share one row: the proportional body font leaves enough
    // room for both, and the battery moves up into the space of the old row.
    snprintf(dbuf, sizeof(dbuf), "%s, %02d.%02d.%04d", wdays[tm.tm_wday],
             tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);

    int jx = (tm.tm_min % 3) - 1;
    int jy = (tm.tm_min / 3 % 3) - 1;

    int y = 70 + draw_time_height() + 6 + jy;
    draw_text_center(dbuf, y, jx, COLOR_WHITE, fb);

#if CLOCK_SHOW_BATTERY
    y += draw_text_height() + 4;
    // The address of the setup page replaces the battery row while the
    // server runs; both want the same row and the address matters more.
    if (ui_status.ip[0] == '\0') draw_battery_row(jx, y + jy, fb);
#endif
    if (ui_status.ip[0] != '\0') draw_setup_ip(ui_status.ip, y + jy, fb);

    ui_status_draw(fb);
}

void clock_render(const uint16_t *background)
{
    // Shared with the slideshow: combined mode passes the very same buffer
    // (nothing to copy then), clock mode passes no background at all.
    uint16_t *fb = display_get_framebuffer();
    if (background && background != fb) {
        for (size_t i = 0; i < 240 * 240; i++) fb[i] = background[i];
    } else if (!background) {
        for (size_t i = 0; i < 240 * 240; i++) fb[i] = RGB565_SWAP(COLOR_BLACK);
    }

    clock_compose(fb);
    esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0, 240, 240, fb);
}