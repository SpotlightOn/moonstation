#include "clock.h"
#include "battery.h"
#include "display.h"
#include "font.h"
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

#if __has_include("config.h")
#include "config.h"
#endif

// The WiFi, netif and SNTP code is always compiled and linked so every
// firmware image ships with the full stack; without credentials below the
// clock simply keeps the compile time and never touches the radio.
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "freertos/event_groups.h"

// WiFi credentials come from config.h; empty means "not configured".
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

// NTP server the clock syncs against; config.h may override it.
#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif

static const char wifi_ssid[] = WIFI_SSID;
static const char wifi_password[] = WIFI_PASSWORD;

// True when config.h supplied WiFi credentials. The check runs at runtime on
// purpose: it keeps wifi_connect() and sntp_sync() referenced, so the linker
// always keeps the WiFi and SNTP stack - also in firmware built without a
// config.h.
static bool wifi_configured(void)
{
    return wifi_ssid[0] != '\0';
}

// Listen interval for max modem sleep, in beacon intervals (about 100 ms
// each). Needed so WIFI_PS_MAX_MODEM actually sleeps longer than the AP DTIM
// period; falls back to the beacon interval when left at 0.
#ifndef WIFI_LISTEN_INTERVAL
#define WIFI_LISTEN_INTERVAL 10
#endif

// Battery indicator below the weekday (0 = off).
#ifndef CLOCK_SHOW_BATTERY
#define CLOCK_SHOW_BATTERY 1
#endif

// Standard (winter) UTC offset in hours for localtime(); decimals allowed.
// Germany = 1 (CET). Must be set before the compile time is interpreted.
#ifndef TIMEZONE_STD_OFFSET_HOURS
#define TIMEZONE_STD_OFFSET_HOURS 1
#endif

// Observe European summer time: last Sunday in March to the last Sunday in
// October (0 keeps the standard offset all year).
#ifndef TIMEZONE_EU_DST
#define TIMEZONE_EU_DST 1
#endif

// Deep sleep window of the clock mode, local hours [FROM, TO). A window that
// ends before it starts crosses midnight (23 -> 7); equal hours disable it.
#ifndef CLOCK_DEEP_SLEEP_FROM
#define CLOCK_DEEP_SLEEP_FROM 23
#endif
#ifndef CLOCK_DEEP_SLEEP_TO
#define CLOCK_DEEP_SLEEP_TO 7
#endif

// Local hour of the daily NTP resync (-1 = off). The boot sync only runs
// once per power cycle, while the RTC drifts by seconds per day; the deep
// sleep wake-up at the end of the night window syncs on its own, so this
// mainly covers boards that stay awake.
#ifndef NTP_DAILY_SYNC_HOUR
#define NTP_DAILY_SYNC_HOUR 4
#endif

// Point localtime() at the configured zone via a POSIX TZ string. The EU
// rules switch at 01:00 UTC, which matches the POSIX default switch times.
static void apply_timezone(void)
{
    // POSIX offsets are inverted: local = UTC - offset.
    int offset_min = -(int)(TIMEZONE_STD_OFFSET_HOURS * 60 +
                            (TIMEZONE_STD_OFFSET_HOURS >= 0 ? 0.5 : -0.5));
    char tz[40];
    int h = offset_min / 60;
    int m = abs(offset_min % 60);
    if (m) {
        snprintf(tz, sizeof(tz), "XXX%d:%02d", h, m);
    } else {
        snprintf(tz, sizeof(tz), "XXX%d", h);
    }
#if TIMEZONE_EU_DST
    strncat(tz, "YYY,M3.5.0/2,M10.5.0/3", sizeof(tz) - strlen(tz) - 1);
#endif
    setenv("TZ", tz, 1);
    tzset();
}

// Charge level at or below which the indicator turns red. Shares the
// threshold with the low battery beep in battery.c.
#ifndef BATTERY_WARN_PCT
#define BATTERY_WARN_PCT 10
#endif

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
    if (CLOCK_DEEP_SLEEP_FROM == CLOCK_DEEP_SLEEP_TO) return false;
    time_t now;
    time(&now);
    struct tm tm;
    localtime_r(&now, &tm);
    if (CLOCK_DEEP_SLEEP_FROM < CLOCK_DEEP_SLEEP_TO) {
        return tm.tm_hour >= CLOCK_DEEP_SLEEP_FROM && tm.tm_hour < CLOCK_DEEP_SLEEP_TO;
    }
    // The window crosses midnight (e.g. 23 -> 7).
    return tm.tm_hour >= CLOCK_DEEP_SLEEP_FROM || tm.tm_hour < CLOCK_DEEP_SLEEP_TO;
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
    tm.tm_hour = CLOCK_DEEP_SLEEP_TO;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t end = mktime(&tm);
    if (end <= now) end += 24 * 60 * 60; // the window crosses midnight
    return (int64_t)(end - now) * 1000000LL;
}

// True once netif, event loop and driver are set up. The daily resync brings
// the radio up a second time, and creating the default netif twice would
// register a second station interface.
static bool wifi_ready = false;

// Brings up the station interface and waits for an address. Returns false
// when the AP does not answer, so the caller can skip the pointless SNTP
// wait instead of burning its timeout on a link that never came up.
static bool wifi_connect(void)
{
    if (!wifi_ready) {
        esp_netif_init();
        esp_event_loop_create_default();
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&cfg);
        wifi_ready = true;
    }
    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid, wifi_ssid, sizeof(wcfg.sta.ssid));
    strncpy((char *)wcfg.sta.password, wifi_password, sizeof(wcfg.sta.password));
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wcfg.sta.listen_interval = WIFI_LISTEN_INTERVAL;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    esp_wifi_start();
    esp_wifi_connect();

    // Wait up to 10 s for an IP.
    for (int i = 0; i < 50; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_netif_ip_info_t ip;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            ESP_LOGI(TAG, "WiFi connected");
            return true;
        }
    }
    ESP_LOGW(TAG, "WiFi connect timeout");
    return false;
}

// Starts SNTP and blocks until the first answer has set the system clock.
// The clock is compiled with and without credentials in a config.h, so the
// wait simply times out and the previous time stays in place when no server
// answers. Returns false on timeout.
static bool sntp_sync(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    // The radio goes off right after this call, so there is nothing to gain
    // from stepping the clock smoothly over minutes.
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_setservername(0, NTP_SERVER);
    esp_sntp_init();
    for (int i = 0; i < 50; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
        // Only the library itself can tell a real sync from a clock that
        // merely looks valid: set_compile_time() already puts the system
        // time far above any epoch threshold.
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            ESP_LOGI(TAG, "NTP synced from %s", NTP_SERVER);
            return true;
        }
    }
    ESP_LOGW(TAG, "NTP sync timeout, keeping the time from before");
    return false;
}

// One sync cycle: radio up, first NTP answer, radio off again. Shared by the
// boot and by the daily resync.
static void ntp_sync_cycle(const char *reason)
{
    if (!wifi_connect()) {
        ESP_LOGW(TAG, "no WiFi for the %s sync, keeping the time from before", reason);
        return;
    }
    sntp_sync();
    esp_sntp_stop(); // stop the poll timer before the radio goes off
    esp_wifi_stop();
    ESP_LOGI(TAG, "WiFi stopped after the %s sync", reason);
}

static esp_timer_handle_t ntp_timer = NULL;

// Microseconds until the next configured sync hour in local time.
static int64_t ntp_sync_delay_us(void)
{
    time_t now;
    time(&now);
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = NTP_DAILY_SYNC_HOUR;
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
    if (NTP_DAILY_SYNC_HOUR < 0 || !wifi_configured()) return;
    const esp_timer_create_args_t timer_args = {
        .callback = ntp_sync_timer_cb,
        .name = "ntp_sync",
    };
    if (esp_timer_create(&timer_args, &ntp_timer) != ESP_OK) {
        ESP_LOGW(TAG, "daily NTP timer unavailable");
        return;
    }
    esp_timer_start_once(ntp_timer, ntp_sync_delay_us());
    ESP_LOGI(TAG, "daily NTP sync armed for %02d:00", NTP_DAILY_SYNC_HOUR);
}

esp_err_t clock_init(void)
{
    apply_timezone(); // before anything interprets wall-clock time
    set_compile_time();
#if CLOCK_SHOW_BATTERY
    if (battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "battery indicator unavailable");
    }
#endif
    bool deep_sleep_wake = esp_sleep_get_wakeup_causes() & (1 << ESP_SLEEP_WAKEUP_TIMER);
    // Deep sleep wake-ups stay radio-free while the sleep window lasts; the
    // first wake outside it syncs via NTP again, so drift cannot pile up.
    if (wifi_configured() && !(deep_sleep_wake && clock_deep_sleep_window_active())) {
        ntp_sync_cycle("boot");
    } else if (wifi_configured()) {
        ESP_LOGI(TAG, "deep sleep wake-up inside the sleep window, skipping WiFi/NTP");
    } else {
        ESP_LOGI(TAG, "no WiFi configured, clock keeps compile time");
    }
    ntp_sync_schedule();
    ESP_LOGI(TAG, "clock ready");
    return ESP_OK;
}

static void draw_text(const char *s, int x, int y, const unsigned char (*glyphs)[54],
                      int gw, int gh, uint16_t color, uint16_t *fb) {
    uint16_t px = RGB565_SWAP(color); // font.h writes pixels as given
    int cx = x;
    for (const char *p = s; *p; p++) {
        int idx = font_date_index(*p);
        if (idx >= 0) {
            font_draw_char(glyphs[idx], gw, gh, cx, y, px, fb);
        }
        cx += gw;
    }
}

static void draw_time_text(const char *s, int x, int y, uint16_t color, uint16_t *fb) {
    uint16_t px = RGB565_SWAP(color);
    int cx = x;
    for (const char *p = s; *p; p++) {
        int idx = font_time_index(*p);
        if (idx >= 0) {
            font_draw_char(font_time_glyphs[idx], font_time_W, font_time_H, cx, y, px, fb);
        }
        cx += font_time_W;
    }
}

#if CLOCK_SHOW_BATTERY
// Solid rectangle clipped to the screen. color is plain RGB565 and is byte
// swapped for the panel on the way into the buffer.
static void fill_rect(int x, int y, int w, int h, uint16_t color, uint16_t *fb) {
    uint16_t px = RGB565_SWAP(color);
    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= 240) continue;
        for (int col = x; col < x + w; col++) {
            if (col < 0 || col >= 240) continue;
            fb[row * 240 + col] = px;
        }
    }
}

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
    } else if (level <= BATTERY_WARN_PCT) {
        color = COLOR_BATT_LOW;
    }

    char pbuf[8];
    snprintf(pbuf, sizeof(pbuf), "%d%%", level);

    int gap = 6;
    int arrow_w = charging ? BATT_ARROW_W + 4 : 0;
    int icon_w = BATT_ICON_W + BATT_NOB_W;
    int total = arrow_w + icon_w + gap + (int)strlen(pbuf) * font_date_W;
    int gx = (240 - total) / 2 + x;

    // The icon is shorter than the text, so center it on the text row.
    int iy = y + (font_date_H - BATT_ICON_H) / 2;

    if (charging) {
        draw_charge_arrow(gx, iy + (BATT_ICON_H - BATT_ARROW_H) / 2, color, fb);
        gx += arrow_w;
    }
    draw_battery_icon(gx, iy, level, color, fb);
    draw_text(pbuf, gx + icon_w + gap, y, font_date_glyphs,
              font_date_W, font_date_H, color, fb);
}
#endif // CLOCK_SHOW_BATTERY

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

    time_t now;
    time(&now);
    struct tm tm;
    localtime_r(&now, &tm);

    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    int tw = strlen(tbuf) * font_time_W;
    draw_time_text(tbuf, (240 - tw) / 2, 70, COLOR_WHITE, fb);

    char wbuf[8];
    char dbuf[64];
    static const char *wdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    snprintf(dbuf, sizeof(dbuf), "%02d.%02d.%04d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
    snprintf(wbuf, sizeof(wbuf), "%s", wdays[tm.tm_wday]);

    int jx = (tm.tm_min % 3) - 1;
    int jy = (tm.tm_min / 3 % 3) - 1;

    int y = 70 + font_time_H + 6 + jy;
    int dw = strlen(dbuf) * font_date_W;
    draw_text(dbuf, (240 - dw) / 2 + jx, y, font_date_glyphs,
              font_date_W, font_date_H, COLOR_WHITE, fb);
    y += font_date_H + 4;
    int ww = strlen(wbuf) * font_date_W;
    draw_text(wbuf, (240 - ww) / 2 + jx, y, font_date_glyphs,
              font_date_W, font_date_H, COLOR_WHITE, fb);

#if CLOCK_SHOW_BATTERY
    y += font_date_H + 4;
    draw_battery_row(jx, y + jy, fb);
#endif

    esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0, 240, 240, fb);
}