#include "weather.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "display.h"
#include "draw.h"
#include "net.h"
#include "screenshot.h"
#include "settings.h"
#include "status.h"
#include "weather_icons.h"

static const char *TAG = "weather";

// The forecast answer for five days is the biggest body of a refresh; the cap
// only protects the heap from a badly framed answer.
#define FETCH_MAX 24576
#define FORECAST_STEPS 8
#define DAILY_DAYS 5
#define CACHE_VERSION 2
#define CACHE_NS "weather"
#define CACHE_KEY "cache"
// A failed refresh (no WiFi, API down, bad key) waits this long before it
// tries again, so a wrong key does not burn the request quota.
#define RETRY_US (5 * 60 * 1000000LL)

#define DEG "\xB0" // degree sign, carried by the text font as 0xB0

// Colors. Same values the icon module uses, kept local so a palette change
// stays a decision of the screen layout.
#define COL_BG 0x0000
#define COL_TEXT 0xFFFF
#define COL_DIM 0x8C71
#define COL_AMBER 0xFD20

typedef struct {
    int32_t dt;      // unix time of the step
    int16_t temp;    // degrees Celsius
    int16_t cond;    // OpenWeather condition id
    uint8_t pop;     // probability of precipitation, percent
} weather_step_t;

// One row of the daily page, aggregated from the 3 hour steps of that day.
typedef struct {
    int32_t dt;      // first step of the day, only used for the weekday
    int16_t temp_min;
    int16_t temp_max;
    int16_t cond;    // condition closest to local noon
    uint8_t pop;     // highest chance of precipitation of the day
} weather_day_t;

// Everything a screen needs. Stored in NVS as one blob, so the version field
// guards against a layout change of an older firmware.
typedef struct {
    uint32_t version;
    int64_t fetched_unix;
    int32_t sunrise;
    int32_t sunset;
    int16_t cond;
    int16_t temp;
    int16_t feels;
    int16_t temp_min;
    int16_t temp_max;
    uint16_t wind_deg;
    uint8_t humidity;
    uint8_t wind_kmh;
    uint8_t pop;   // highest chance of rain in the forecast window
    char city[24];
    char desc[32];
    weather_step_t steps[FORECAST_STEPS];
    weather_day_t days[DAILY_DAYS];
} weather_data_t;

static weather_data_t wx;
static bool have_data;
static int page;                  // 0 current, 1 next 24 h, 2 next days
static volatile bool tap_pending; // set in the touch task, handled on wake
static bool failed;               // last refresh failed, waiting for RETRY_US
static int64_t retry_at_us;

//
// Cache
//

static void cache_save(void)
{
    nvs_handle_t handle;
    if (nvs_open(CACHE_NS, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_blob(handle, CACHE_KEY, &wx, sizeof(wx));
    nvs_commit(handle);
    nvs_close(handle);
}

esp_err_t weather_init(void)
{
    memset(&wx, 0, sizeof(wx));
    page = 0;
    tap_pending = false;
    failed = false;
    retry_at_us = 0;

    nvs_handle_t handle;
    if (nvs_open(CACHE_NS, NVS_READONLY, &handle) == ESP_OK) {
        weather_data_t cached;
        size_t len = sizeof(cached);
        esp_err_t err = nvs_get_blob(handle, CACHE_KEY, &cached, &len);
        nvs_close(handle);
        if (err == ESP_OK && len == sizeof(cached) &&
            cached.version == CACHE_VERSION && cached.fetched_unix > 0) {
            wx = cached;
            have_data = true;
            ESP_LOGI(TAG, "cached reading from %lld", (long long)wx.fetched_unix);
        }
    }
    return ESP_OK;
}

//
// Request
//

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_buf_t;

static esp_err_t http_event(esp_http_client_event_t *ev)
{
    http_buf_t *b = ev->user_data;
    if (ev->event_id != HTTP_EVENT_ON_DATA || ev->data_len <= 0) return ESP_OK;

    size_t need = b->len + (size_t)ev->data_len + 1;
    if (need > FETCH_MAX) return ESP_FAIL; // abort the transfer
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < need) cap *= 2;
        if (cap > FETCH_MAX) cap = FETCH_MAX;
        char *grown = realloc(b->buf, cap);
        if (grown == NULL) return ESP_FAIL;
        b->buf = grown;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, ev->data, (size_t)ev->data_len);
    b->len += (size_t)ev->data_len;
    b->buf[b->len] = '\0';
    return ESP_OK;
}

// One GET into a heap buffer. The URL carries the API key, so it never shows
// up in a log line; callers log the endpoint on their own.
static bool http_get(const char *url, char **out)
{
    http_buf_t buf = {0};
    const esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
        .user_data = &buf,
        .timeout_ms = 8000,
        .buffer_size = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        free(buf.buf);
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || buf.buf == NULL) {
        ESP_LOGW(TAG, "request failed: %s, http %d", esp_err_to_name(err), status);
        free(buf.buf);
        return false;
    }
    *out = buf.buf;
    return true;
}

static bool build_url(char *out, size_t cap, const char *endpoint, const char *extra)
{
    int n = snprintf(out, cap,
                     "https://api.openweathermap.org/data/2.5/%s"
                     "?lat=%s&lon=%s&units=metric&appid=%s%s",
                     endpoint, settings.weather_lat, settings.weather_lon,
                     settings.weather_api_key, extra);
    return n > 0 && (size_t)n < cap;
}

static int json_int(const cJSON *obj, const char *key, int dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(v)) return dflt;
    double d = v->valuedouble;
    return (int)(d < 0 ? d - 0.5 : d + 0.5);
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
}

static bool parse_current(const char *body, weather_data_t *out)
{
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) return false;

    const cJSON *main_obj = cJSON_GetObjectItemCaseSensitive(root, "main");
    const cJSON *weather = cJSON_GetObjectItemCaseSensitive(root, "weather");
    const cJSON *w0 = cJSON_GetArrayItem(weather, 0);
    const cJSON *wind = cJSON_GetObjectItemCaseSensitive(root, "wind");
    const cJSON *sys = cJSON_GetObjectItemCaseSensitive(root, "sys");

    bool ok = cJSON_IsObject(main_obj) && cJSON_IsObject(w0);
    if (ok) {
        out->cond = (int16_t)json_int(w0, "id", 800);
        copy_str(out->desc, sizeof(out->desc), json_str(w0, "description"));
        if (out->desc[0] >= 'a' && out->desc[0] <= 'z') out->desc[0] -= 'a' - 'A';
        copy_str(out->city, sizeof(out->city), json_str(root, "name"));
        out->temp = (int16_t)json_int(main_obj, "temp", 0);
        out->feels = (int16_t)json_int(main_obj, "feels_like", out->temp);
        out->temp_min = (int16_t)json_int(main_obj, "temp_min", out->temp);
        out->temp_max = (int16_t)json_int(main_obj, "temp_max", out->temp);
        out->humidity = (uint8_t)json_int(main_obj, "humidity", 0);
        int kmh = json_int(wind, "speed", 0) * 36 / 10; // m/s, rounded once
        out->wind_kmh = (uint8_t)(kmh > 255 ? 255 : kmh);
        out->wind_deg = (uint16_t)json_int(wind, "deg", 0);
        out->sunrise = json_int(sys, "sunrise", 0);
        out->sunset = json_int(sys, "sunset", 0);
    }
    cJSON_Delete(root);
    return ok;
}

static bool parse_forecast(const char *body, weather_data_t *out)
{
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) return false;

    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "list");
    int count = cJSON_GetArraySize(list);
    int used = 0;
    int day_count = 0;
    int day_key[DAILY_DAYS];
    int noon_gap[DAILY_DAYS];

    for (int i = 0; i < count; i++) {
        const cJSON *item = cJSON_GetArrayItem(list, i);
        const cJSON *temp = cJSON_GetObjectItemCaseSensitive(item, "main");
        const cJSON *w0 = cJSON_GetArrayItem(
            cJSON_GetObjectItemCaseSensitive(item, "weather"), 0);
        if (!cJSON_IsObject(temp) || !cJSON_IsObject(w0)) continue;

        int32_t dt = json_int(item, "dt", 0);
        int16_t t = (int16_t)json_int(temp, "temp", 0);
        int16_t cond = (int16_t)json_int(w0, "id", 800);
        const cJSON *pop = cJSON_GetObjectItemCaseSensitive(item, "pop");
        uint8_t chance = cJSON_IsNumber(pop)
                             ? (uint8_t)(pop->valuedouble * 100.0 + 0.5)
                             : 0;

        // The 24 hour page takes the first eight steps; its rain figure must
        // come from those, not from the whole five day window.
        if (used < FORECAST_STEPS) {
            weather_step_t *step = &out->steps[used++];
            step->dt = dt;
            step->temp = t;
            step->cond = cond;
            step->pop = chance;
            if (chance > out->pop) out->pop = chance;
        }

        time_t when = dt;
        struct tm tm;
        localtime_r(&when, &tm);
        int key = tm.tm_yday + 400 * tm.tm_year; // one calendar day, local time
        int gap = tm.tm_hour >= 12 ? tm.tm_hour - 12 : 12 - tm.tm_hour;

        int slot = -1;
        for (int d = 0; d < day_count; d++) {
            if (day_key[d] == key) {
                slot = d;
                break;
            }
        }
        if (slot < 0) {
            if (day_count == DAILY_DAYS) continue; // first days are full
            slot = day_count++;
            day_key[slot] = key;
            noon_gap[slot] = gap;
            weather_day_t *day = &out->days[slot];
            day->dt = dt;
            day->temp_min = t;
            day->temp_max = t;
            day->cond = cond;
            day->pop = chance;
        }
        weather_day_t *day = &out->days[slot];
        if (t < day->temp_min) day->temp_min = t;
        if (t > day->temp_max) day->temp_max = t;
        if (chance > day->pop) day->pop = chance;
        // The icon stands for the day, so the step closest to noon wins over
        // the morning or evening one.
        if (gap < noon_gap[slot]) {
            noon_gap[slot] = gap;
            day->cond = cond;
        }
    }
    cJSON_Delete(root);
    return used > 0;
}

// Runs the two requests and replaces the cache when both answered. WiFi is
// brought up only for the duration (and only when it is down: the setup
// server may hold an established link, and restarting the station under it
// would kill the page).
static bool refresh(void)
{
    if (settings.weather_api_key[0] == '\0') return false;

    bool started_radio = false;
    if (!net_wifi_is_up()) {
        if (!net_wifi_up()) {
            ESP_LOGW(TAG, "no WiFi, keeping the last reading");
            return false;
        }
        started_radio = true;
    }

    char url[256];
    char *body = NULL;
    weather_data_t fresh = {0};
    fresh.version = CACHE_VERSION;
    bool ok = false;

    if (!build_url(url, sizeof(url), "weather", "")) goto out;
    if (!http_get(url, &body)) goto out;
    if (!parse_current(body, &fresh)) {
        ESP_LOGW(TAG, "current: unreadable answer");
        goto out;
    }
    free(body);
    body = NULL;

    // 40 steps of three hours are five days; the first eight of them are the
    // 24 hour page and all of them feed the daily rows.
    if (!build_url(url, sizeof(url), "forecast", "&cnt=40")) goto out;
    if (!http_get(url, &body)) goto out;
    if (!parse_forecast(body, &fresh)) {
        ESP_LOGW(TAG, "forecast: unreadable answer");
        goto out;
    }

    fresh.fetched_unix = (int64_t)time(NULL);
    wx = fresh;
    have_data = true;
    cache_save();
    ok = true;
    ESP_LOGI(TAG, "refreshed, %d%%, %d km/h", wx.humidity, wx.wind_kmh);

out:
    free(body);
    if (started_radio) net_wifi_down();
    return ok;
}

// The TLS handshake needs far more stack than the UI task has (the HTTPS
// setup server runs with 10 kB as well), so the request runs in a worker with
// its own stack and the caller waits for the result. Without this the first
// successful request crashed the main task with a stack overflow.
#define REFRESH_STACK 12288

static TaskHandle_t refresh_waiter;
static bool refresh_result;

static void refresh_task(void *arg)
{
    (void)arg;
    refresh_result = refresh();
    xTaskNotifyGive(refresh_waiter);
    vTaskDelete(NULL);
}

static bool refresh_in_task(void)
{
    refresh_waiter = xTaskGetCurrentTaskHandle();
    ulTaskNotifyTake(pdTRUE, 0); // drop a notification left by an earlier run
    if (xTaskCreate(refresh_task, "wx_refresh", REFRESH_STACK, NULL, 4, NULL) !=
        pdPASS) {
        ESP_LOGW(TAG, "no stack for the request");
        return false;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return refresh_result;
}

//
// Drawing
//

static bool is_stale(void)
{
    if (!have_data) return false;
    int64_t age = (int64_t)time(NULL) - wx.fetched_unix;
    return age > 2LL * settings.weather_refresh_min * 60;
}

static bool is_night(int32_t dt)
{
    if (wx.sunrise > 0 && wx.sunset > wx.sunrise) {
        return dt < wx.sunrise || dt >= wx.sunset;
    }
    // No sunrise in the answer: fall back to a rough local-time guess.
    time_t t = dt;
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour < 6 || tm.tm_hour >= 21;
}

// Shortens a line to max_w pixels, marking the cut with two dots.
static void fit_text(char *dst, size_t cap, const char *src, int max_w)
{
    snprintf(dst, cap, "%s", src);
    size_t n = strlen(dst);
    if (draw_text_width(dst) <= max_w) return;
    while (n > 2 && draw_text_width(dst) > max_w) {
        n--;
        dst[n] = '\0';
    }
    if (n >= 2) {
        dst[n - 2] = '.';
        dst[n - 1] = '.';
        dst[n] = '\0';
    } else {
        dst[0] = '\0';
    }
}

static void draw_centered_fit(const char *src, int y, int max_w, uint16_t color,
                              uint16_t *fb)
{
    char line[64];
    fit_text(line, sizeof(line), src, max_w);
    draw_text_center(line, y, 0, color, fb);
}

static void draw_stale_marker(int y, uint16_t *fb)
{
    if (!is_stale()) return;
    draw_ring(120, y, 3, 1, COL_AMBER, fb);
}

static const char *compass(uint16_t deg)
{
    static const char *names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return names[((deg + 22) / 45) % 8];
}

static void push(uint16_t *fb)
{
    ui_status_draw(fb);
    esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0, SCREEN_W, SCREEN_H, fb);
}

static void render_message(const char *title, const char *hint, uint16_t *fb)
{
    fill_rect(0, 0, SCREEN_W, SCREEN_H, COL_BG, fb);
    weather_icon_draw(120, 84, 60, WEATHER_ICON_CLOUD, false, fb);
    draw_centered_fit(title, 122, 190, COL_TEXT, fb);
    if (hint) draw_centered_fit(hint, 146, 190, COL_DIM, fb);
    push(fb);
}

static void render_current(uint16_t *fb)
{
    fill_rect(0, 0, SCREEN_W, SCREEN_H, COL_BG, fb);
    draw_stale_marker(12, fb);

    bool night = is_night((int32_t)time(NULL));
    weather_icon_draw(52, 100, 52, weather_icon_kind(wx.cond), night, fb);

    // Big temperature in the clock font, with the degree mark as a small ring
    // after the digits, the way the current design showed it.
    char big[8];
    snprintf(big, sizeof(big), "%d", wx.temp);
    int tw = draw_time_width(big);
    int group = tw + 10 + 9;
    int x = 144 - group / 2;
    draw_time_text(big, x, 62, COL_TEXT, fb);
    draw_ring(x + tw + 10 + 4, 62 + 12, 4, 2, COL_TEXT, fb);

    if (wx.city[0]) draw_centered_fit(wx.city, 26, 148, COL_DIM, fb);
    draw_centered_fit(wx.desc, 126, 188, COL_TEXT, fb);

    char line[48];
    snprintf(line, sizeof(line), "Feels like %d" DEG, wx.feels);
    draw_text_center(line, 145, 0, COL_DIM, fb);

    snprintf(line, sizeof(line), "H %d" DEG "   L %d" DEG, wx.temp_max, wx.temp_min);
    draw_text_center(line, 164, 0, COL_TEXT, fb);

    snprintf(line, sizeof(line), "%d%%   %d km/h %s", wx.humidity, wx.wind_kmh,
             compass(wx.wind_deg));
    draw_text_center(line, 182, 0, COL_DIM, fb);

    push(fb);
}

static void render_forecast(uint16_t *fb)
{
    fill_rect(0, 0, SCREEN_W, SCREEN_H, COL_BG, fb);
    draw_stale_marker(6, fb);

    char header[32];
    if (wx.pop > 0) snprintf(header, sizeof(header), "24 h   rain %d%%", wx.pop);
    else snprintf(header, sizeof(header), "Next 24 h");
    draw_text_center(header, 16, 0, COL_DIM, fb);

    // Four columns per row, 44 px apart and centered on the panel.
    for (int i = 0; i < FORECAST_STEPS; i++) {
        const weather_step_t *step = &wx.steps[i];
        if (step->dt == 0) break;
        int col = i % 4;
        int row = i / 4;
        int x = 54 + col * 44;
        int label_y = row ? 122 : 44;
        int icon_y = row ? 156 : 78;
        int temp_y = row ? 176 : 98;

        time_t t = step->dt;
        struct tm tm;
        localtime_r(&t, &tm);
        char label[8];
        snprintf(label, sizeof(label), "%02d:%02d", tm.tm_hour, tm.tm_min);
        int w = draw_text_width(label);
        draw_text(label, x - w / 2, label_y, COL_DIM, fb);

        weather_icon_draw(x, icon_y, 26, weather_icon_kind(step->cond),
                          is_night(step->dt), fb);

        char temp[8];
        snprintf(temp, sizeof(temp), "%d" DEG, step->temp);
        w = draw_text_width(temp);
        draw_text(temp, x - w / 2, temp_y, COL_TEXT, fb);
    }

    push(fb);
}

static void render_daily(uint16_t *fb)
{
    fill_rect(0, 0, SCREEN_W, SCREEN_H, COL_BG, fb);
    draw_stale_marker(6, fb);

    char header[32];
    snprintf(header, sizeof(header), "Next %d days", DAILY_DAYS);
    draw_text_center(header, 16, 0, COL_DIM, fb);

    static const char *const weekday[] = {"Sun", "Mon", "Tue", "Wed",
                                          "Thu", "Fri", "Sat"};
    for (int i = 0; i < DAILY_DAYS; i++) {
        const weather_day_t *day = &wx.days[i];
        if (day->dt == 0) break;
        // Five rows of 28 px between the header and the lower edge.
        int y = 52 + i * 28;
        time_t t = day->dt;
        struct tm tm;
        localtime_r(&t, &tm);

        draw_text(weekday[tm.tm_wday], 44, y, COL_TEXT, fb);
        weather_icon_draw(88, y + 9, 22, weather_icon_kind(day->cond), false,
                          fb);

        char temp[8];
        snprintf(temp, sizeof(temp), "%d" DEG, day->temp_max);
        draw_text(temp, 112, y, COL_TEXT, fb);
        snprintf(temp, sizeof(temp), "%d" DEG, day->temp_min);
        draw_text(temp, 146, y, COL_DIM, fb);

        // Only worth the space when it is a real chance; the 24 hour page
        // carries the detailed figure in its header. Right-aligned so the
        // widest case, "100%", still sits inside the round panel up here.
        if (day->pop >= 20) {
            char pop[8];
            snprintf(pop, sizeof(pop), "%d%%", day->pop);
            draw_text(pop, 220 - draw_text_width(pop), y, COL_DIM, fb);
        }
    }

    push(fb);
}

void weather_redraw(void)
{
    uint16_t *fb = display_get_framebuffer();
    if (settings.weather_api_key[0] == '\0') {
        render_message("No API key", "Set it on the setup page", fb);
    } else if (!have_data) {
        const char *hint = "Requesting weather data";
        if (failed) {
            hint = net_wifi_configured() ? "The last request failed"
                                         : "Set WiFi on the setup page";
        }
        render_message("No weather yet", hint, fb);
    } else if (page == 0) {
        render_current(fb);
    } else if (page == 1) {
        render_forecast(fb);
    } else {
        render_daily(fb);
    }
}

//
// Mode callbacks
//

void weather_enter(void)
{
    page = 0;
    tap_pending = false;
    // A fresh visit may retry right away, even after a failure.
    failed = false;
    retry_at_us = 0;
    weather_redraw();
}

void weather_on_tap(void)
{
    tap_pending = true; // touch task, the redraw happens on the next wake
}

static bool refresh_due(void)
{
    if (settings.weather_api_key[0] == '\0') return false;
    if (failed && esp_timer_get_time() < retry_at_us) return false;
    if (failed) return true;
    if (!have_data) return true;
    int64_t age = (int64_t)time(NULL) - wx.fetched_unix;
    // A clock that is behind the stored reading (fresh boot before NTP) must
    // not hide the request: a negative age counts as due.
    if (age < 0) return true;
    return age >= (int64_t)settings.weather_refresh_min * 60;
}

void weather_on_wake(void)
{
    bool redraw = false;
    bool flipped = false;
    if (tap_pending) {
        tap_pending = false;
        page = (page + 1) % 3;
        flipped = true;
        redraw = true;
    }
    if (refresh_due()) {
        bool ok = refresh_in_task();
        if (!ok) {
            failed = true;
            retry_at_us = esp_timer_get_time() + RETRY_US;
        } else {
            failed = false;
            retry_at_us = 0;
        }
        redraw = true;
    }
    if (redraw) {
        weather_redraw();
        // A tap is the only moment the page changes without a mode switch,
        // so this is where the debug dump needs a name per page. Refreshes
        // stay quiet, otherwise a 20 minute interval would spam the card.
        if (flipped) {
            screenshot_capture(page == 0 ? "wx-cur"
                                         : page == 1 ? "wx-fc" : "wx-days");
        }
    }
}

int64_t weather_next_refresh_us(void)
{
    if (settings.weather_api_key[0] == '\0') return 3600000000LL;
    if (failed) {
        int64_t left = retry_at_us - esp_timer_get_time();
        return left > 0 ? left : 0;
    }
    if (!have_data) return 0;
    int64_t age = (int64_t)time(NULL) - wx.fetched_unix;
    int64_t period = (int64_t)settings.weather_refresh_min * 60;
    int64_t left = period - age;
    if (left <= 0) return 0;
    return left * 1000000LL;
}
