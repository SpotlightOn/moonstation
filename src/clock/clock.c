#include "clock.h"
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
    int cx = x;
    for (const char *p = s; *p; p++) {
        int idx = font_date_index(*p);
        if (idx >= 0) {
            font_draw_char(glyphs[idx], gw, gh, cx, y, color, fb);
        }
        cx += gw;
    }
}

static void draw_time_text(const char *s, int x, int y, uint16_t color, uint16_t *fb) {
    int cx = x;
    for (const char *p = s; *p; p++) {
        int idx = font_time_index(*p);
        if (idx >= 0) {
            font_draw_char(font_time_glyphs[idx], font_time_W, font_time_H, cx, y, color, fb);
        }
        cx += font_time_W;
    }
}

void clock_render(const uint16_t *background)
{
    static uint16_t fb[240 * 240];
    if (background) {
        for (size_t i = 0; i < 240 * 240; i++) fb[i] = background[i];
    } else {
        for (size_t i = 0; i < 240 * 240; i++) fb[i] = 0x0000;
    }

    time_t now;
    time(&now);
    struct tm tm;
    localtime_r(&now, &tm);

    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    int tw = strlen(tbuf) * font_time_W;
    draw_time_text(tbuf, (240 - tw) / 2, 70, 0xFFFF, fb);

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
              font_date_W, font_date_H, 0xFFFF, fb);
    y += font_date_H + 4;
    int ww = strlen(wbuf) * font_date_W;
    draw_text(wbuf, (240 - ww) / 2 + jx, y, font_date_glyphs,
              font_date_W, font_date_H, 0xFFFF, fb);

    esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0, 240, 240, fb);
}