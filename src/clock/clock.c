#include "clock.h"
#include "battery.h"
#include "display.h"
#include "font.h"
#include "esp_log.h"
#include "esp_err.h"
#include "sys/time.h"
#include "time.h"
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if __has_include("config.h")
#include "config.h"
#endif

#if defined(WIFI_SSID)
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "freertos/event_groups.h"

// Listen interval for max modem sleep, in beacon intervals (about 100 ms
// each). Needed so WIFI_PS_MAX_MODEM actually sleeps longer than the AP DTIM
// period; falls back to the beacon interval when left at 0.
#ifndef WIFI_LISTEN_INTERVAL
#define WIFI_LISTEN_INTERVAL 10
#endif
#endif

// Battery indicator below the weekday (0 = off).
#ifndef CLOCK_SHOW_BATTERY
#define CLOCK_SHOW_BATTERY 1
#endif

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
#define COLOR_BATT_CHARGING 0x07E0 // green
#define COLOR_BATT_NORMAL 0xFFFF   // white
#define COLOR_WHITE 0xFFFF         // white
#define COLOR_BLACK 0x0000         // black

static const char *TAG = "clock";

static void set_compile_time(void)
{
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
    time_t now = mktime(&t);
    struct timeval tv = { .tv_sec = now, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

#if defined(WIFI_SSID)
static void wifi_connect(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid, WIFI_SSID, sizeof(wcfg.sta.ssid));
#ifdef WIFI_PASSWORD
    strncpy((char *)wcfg.sta.password, WIFI_PASSWORD, sizeof(wcfg.sta.password));
#endif
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
            return;
        }
    }
    ESP_LOGW(TAG, "WiFi connect timeout");
}

static void sntp_sync(void)
{
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
    for (int i = 0; i < 50; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
        time_t now = 0;
        time(&now);
        if (now > 1700000000) {
            ESP_LOGI(TAG, "NTP synced");
            return;
        }
    }
    ESP_LOGW(TAG, "NTP sync timeout");
}
#endif

esp_err_t clock_init(void)
{
    set_compile_time();
#if CLOCK_SHOW_BATTERY
    if (battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "battery indicator unavailable");
    }
#endif
#if defined(WIFI_SSID)
    wifi_connect();
    sntp_sync();
    esp_wifi_stop();
    esp_wifi_deinit();
    ESP_LOGI(TAG, "WiFi stopped and deinitialized for power saving");
#endif
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