#include "settings.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"

#if __has_include("config.h")
#include "config.h"
#endif

// Built-in defaults for every setting: config.h overrides them, NVS overrides
// both. Keep in sync with config.h.example - every new setting needs a macro
// there plus an #ifndef fallback here, so a build without config.h works.

#ifndef SLIDE_INTERVAL_MS
#define SLIDE_INTERVAL_MS 10000
#endif
#ifndef SLIDE_DIR
#define SLIDE_DIR "/slides"
#endif
// Crossfade between two images in milliseconds, 0 cuts hard. Needs the PSRAM
// buffer below; without PSRAM the firmware falls back to the hard cut.
#ifndef SLIDE_FADE_MS
#define SLIDE_FADE_MS 0
#endif

#ifndef BACKLIGHT_FULL_PCT
#define BACKLIGHT_FULL_PCT 100
#endif
#ifndef BACKLIGHT_DIM_PCT
#define BACKLIGHT_DIM_PCT 20
#endif
#ifndef BACKLIGHT_DEEP_PCT
#define BACKLIGHT_DEEP_PCT 5
#endif
#ifndef BACKLIGHT_DIM_AFTER_SEC
#define BACKLIGHT_DIM_AFTER_SEC 15
#endif
#ifndef BACKLIGHT_DEEP_AFTER_SEC
#define BACKLIGHT_DEEP_AFTER_SEC 60
#endif

#ifndef CLOCK_DEEP_SLEEP_FROM
#define CLOCK_DEEP_SLEEP_FROM 23
#endif
#ifndef CLOCK_DEEP_SLEEP_TO
#define CLOCK_DEEP_SLEEP_TO 7
#endif
// The compile switch decides whether the deep sleep code is built in and
// doubles as the default of the runtime setting of the same name.
#ifndef CLOCK_DEEP_SLEEP
#define CLOCK_DEEP_SLEEP 0
#endif

#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif
#ifndef NTP_DAILY_SYNC_HOUR
#define NTP_DAILY_SYNC_HOUR 4
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef WIFI_LISTEN_INTERVAL
#define WIFI_LISTEN_INTERVAL 10
#endif

// Standard (winter) UTC offset in hours, decimals allowed; summer time adds
// one hour on top when TIMEZONE_EU_DST is set. Stored as minutes east of UTC.
#ifndef TIMEZONE_STD_OFFSET_HOURS
#define TIMEZONE_STD_OFFSET_HOURS 1
#endif
#ifndef TIMEZONE_EU_DST
#define TIMEZONE_EU_DST 1
#endif

#ifndef BATTERY_WARN_PCT
#define BATTERY_WARN_PCT 10
#endif
#ifndef BATTERY_REFRESH_SEC
#define BATTERY_REFRESH_SEC 300
#endif
#ifndef BATTERY_SAMPLE_COUNT
#define BATTERY_SAMPLE_COUNT 3
#endif
#ifndef BATTERY_CHARGING_ACTIVE_HIGH
#define BATTERY_CHARGING_ACTIVE_HIGH 1
#endif
#ifndef BATTERY_BEEP_VOLUME
#define BATTERY_BEEP_VOLUME 30
#endif

#ifndef AUDIO_VOLUME
#define AUDIO_VOLUME 60
#endif

#ifndef WEB_USER
#define WEB_USER "admin"
#endif
#ifndef WEB_PASSWORD
#define WEB_PASSWORD ""
#endif
#ifndef WEB_IDLE_TIMEOUT_MIN
#define WEB_IDLE_TIMEOUT_MIN 15
#endif

#ifndef LCD_SPI_HZ
#define LCD_SPI_HZ 40000000
#endif

// Weather mode: OpenWeather key, position and refresh interval. The key is
// only a seed for NVS, the setup page stores the one the box really uses.
// The coordinates are stringified because they end up in the request URL.
#ifndef WEATHER_API_KEY
#define WEATHER_API_KEY ""
#endif
#ifndef WEATHER_LAT
#define WEATHER_LAT 52.5200
#endif
#ifndef WEATHER_LON
#define WEATHER_LON 13.4050
#endif
#ifndef WEATHER_REFRESH_MIN
#define WEATHER_REFRESH_MIN 20
#endif

#define SETTINGS_STR_(x) #x
#define SETTINGS_STR(x) SETTINGS_STR_(x)
#define WEATHER_LAT_DEFAULT SETTINGS_STR(WEATHER_LAT)
#define WEATHER_LON_DEFAULT SETTINGS_STR(WEATHER_LON)

#define TZ_STD_OFFSET_MIN \
    ((int16_t)(TIMEZONE_STD_OFFSET_HOURS * 60 + \
               (TIMEZONE_STD_OFFSET_HOURS >= 0 ? 0.5 : -0.5)))

static const char *TAG = "settings";
static const char *NVS_NAMESPACE = "settings";
static const char *NVS_KEY = "config";

// Stored as one versioned blob: bump it whenever the struct layout changes.
// The loader copies whatever length is stored over the defaults, so appended
// fields read as their build-time value instead of zero - see settings_init().
#define SETTINGS_VERSION 5

typedef struct {
    uint32_t version;
    settings_t values;
} settings_blob_t;

settings_t settings;

void settings_defaults_into(settings_t *out)
{
    const settings_t d = {
        .slide_interval_ms = SLIDE_INTERVAL_MS,
        .slide_dir = SLIDE_DIR,
        .slide_fade_ms = SLIDE_FADE_MS,
        .backlight_full_pct = BACKLIGHT_FULL_PCT,
        .backlight_dim_pct = BACKLIGHT_DIM_PCT,
        .backlight_deep_pct = BACKLIGHT_DEEP_PCT,
        .backlight_dim_after_sec = BACKLIGHT_DIM_AFTER_SEC,
        .backlight_deep_after_sec = BACKLIGHT_DEEP_AFTER_SEC,
        .clock_deep_sleep_from = CLOCK_DEEP_SLEEP_FROM,
        .clock_deep_sleep_to = CLOCK_DEEP_SLEEP_TO,
        .clock_deep_sleep_enable = CLOCK_DEEP_SLEEP,
        .ntp_daily_sync_hour = NTP_DAILY_SYNC_HOUR,
        .ntp_server = NTP_SERVER,
        .wifi_ssid = WIFI_SSID,
        .wifi_password = WIFI_PASSWORD,
        .wifi_listen_interval = WIFI_LISTEN_INTERVAL,
        .tz_std_offset_min = TZ_STD_OFFSET_MIN,
        .tz_eu_dst = TIMEZONE_EU_DST,
        .battery_warn_pct = BATTERY_WARN_PCT,
        .battery_refresh_sec = BATTERY_REFRESH_SEC,
        .battery_sample_count = BATTERY_SAMPLE_COUNT,
        .battery_charging_active_high = BATTERY_CHARGING_ACTIVE_HIGH,
        .battery_beep_volume = BATTERY_BEEP_VOLUME,
        .audio_volume = AUDIO_VOLUME,
        .lcd_spi_hz = LCD_SPI_HZ,
        .web_user = WEB_USER,
        .web_password = WEB_PASSWORD,
        .web_idle_timeout_min = WEB_IDLE_TIMEOUT_MIN,
        .weather_api_key = WEATHER_API_KEY,
        .weather_lat = WEATHER_LAT_DEFAULT,
        .weather_lon = WEATHER_LON_DEFAULT,
        .weather_refresh_min = WEATHER_REFRESH_MIN,
    };
    *out = d;
}

void settings_defaults(void)
{
    settings_defaults_into(&settings);
}

// Keeps a broken or hand edited store from taking the firmware down: values
// are clamped to what the code can work with.
// A coordinate has to survive strtod() unchanged and stay inside the valid
// range; anything else falls back to the build-time value.
static void sanitize_coord(char *s, size_t cap, double lo, double hi, const char *dflt)
{
    char *end = NULL;
    double v = strtod(s, &end);
    if (s[0] == '\0' || end == s || *end != '\0' || v < lo || v > hi) {
        snprintf(s, cap, "%s", dflt);
    }
}

static void sanitize(void)
{
    if (settings.slide_interval_ms < 1000) settings.slide_interval_ms = 1000;
    if (settings.slide_dir[0] == '\0') {
        snprintf(settings.slide_dir, sizeof(settings.slide_dir), "%s", SLIDE_DIR);
    }
    if (settings.backlight_full_pct > 100) settings.backlight_full_pct = 100;
    if (settings.backlight_dim_pct > 100) settings.backlight_dim_pct = 100;
    if (settings.backlight_deep_pct > settings.backlight_dim_pct)
        settings.backlight_deep_pct = settings.backlight_dim_pct;
    if (settings.backlight_deep_after_sec < settings.backlight_dim_after_sec)
        settings.backlight_deep_after_sec = settings.backlight_dim_after_sec;
    if (settings.clock_deep_sleep_from > 23) settings.clock_deep_sleep_from = 23;
    if (settings.clock_deep_sleep_to > 23) settings.clock_deep_sleep_to = 23;
    if (settings.ntp_daily_sync_hour > 23) settings.ntp_daily_sync_hour = -1;
    if (settings.ntp_daily_sync_hour < -1) settings.ntp_daily_sync_hour = -1;
    if (settings.ntp_server[0] == '\0') {
        snprintf(settings.ntp_server, sizeof(settings.ntp_server), "%s", NTP_SERVER);
    }
    if (settings.wifi_listen_interval > 1000) settings.wifi_listen_interval = 10;
    if (settings.tz_std_offset_min > 14 * 60) settings.tz_std_offset_min = 14 * 60;
    if (settings.tz_std_offset_min < -12 * 60) settings.tz_std_offset_min = -12 * 60;
    if (settings.battery_refresh_sec < 5) settings.battery_refresh_sec = 5;
    if (settings.battery_sample_count < 1) settings.battery_sample_count = 1;
    if (settings.battery_warn_pct > 100) settings.battery_warn_pct = 100;
    if (settings.battery_beep_volume > 100) settings.battery_beep_volume = 100;
    if (settings.audio_volume > 100) settings.audio_volume = 100;
    if (settings.lcd_spi_hz < 1000000) settings.lcd_spi_hz = LCD_SPI_HZ;
    // An empty user would lock the page if a password is set at all.
    if (settings.web_user[0] == '\0') {
        snprintf(settings.web_user, sizeof(settings.web_user), "%s", WEB_USER);
    }
    if (settings.web_user[0] == '\0') {
        snprintf(settings.web_user, sizeof(settings.web_user), "admin");
    }
    // Below one minute the server would shut down while the page is open.
    if (settings.web_idle_timeout_min < 1) settings.web_idle_timeout_min = 1;
    if (settings.clock_deep_sleep_enable > 1) settings.clock_deep_sleep_enable = 1;
    if (settings.slide_fade_ms > 2000) settings.slide_fade_ms = 2000;
    if (settings.weather_refresh_min < 5) settings.weather_refresh_min = 5;
    if (settings.weather_refresh_min > 1440) settings.weather_refresh_min = 1440;
    sanitize_coord(settings.weather_lat, sizeof(settings.weather_lat), -90.0, 90.0,
                   WEATHER_LAT_DEFAULT);
    sanitize_coord(settings.weather_lon, sizeof(settings.weather_lon), -180.0, 180.0,
                   WEATHER_LON_DEFAULT);
}

esp_err_t settings_init(void)
{
    settings_defaults();

    bool loaded = false;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        settings_blob_t blob = {0};
        size_t len = sizeof(blob);
        esp_err_t err = nvs_get_blob(handle, NVS_KEY, &blob, &len);
        nvs_close(handle);
        // A blob from an older firmware is shorter by the members appended
        // since. Copying only what was stored leaves the rest at the defaults
        // set above, which is exactly the migration those fields need.
        const size_t header = offsetof(settings_blob_t, values);
        if (err == ESP_OK && len > header && len <= sizeof(blob) &&
            blob.version <= SETTINGS_VERSION) {
            size_t values_len = len - header;
            if (values_len > sizeof(settings_t)) values_len = sizeof(settings_t);
            memcpy(&settings, &blob.values, values_len);
            loaded = true;
        }
    }

    sanitize();
    if (loaded) {
        ESP_LOGI(TAG, "stored settings loaded");
    } else {
        ESP_LOGI(TAG, "no stored settings, using the defaults");
    }
    return ESP_OK;
}

esp_err_t settings_save(void)
{
    sanitize();

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "settings not stored: %s", esp_err_to_name(err));
        return err;
    }

    settings_blob_t blob = { .version = SETTINGS_VERSION, .values = settings };
    err = nvs_set_blob(handle, NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "settings stored");
    } else {
        ESP_LOGW(TAG, "settings not stored: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t settings_reset_defaults(void)
{
    // The WiFi credentials and the OpenWeather key belong to this box and
    // were entered by hand, the build time values for them are templates at
    // best. Everything else goes back to the configuration the firmware was
    // built with.
    char ssid[sizeof(settings.wifi_ssid)];
    char password[sizeof(settings.wifi_password)];
    char key[sizeof(settings.weather_api_key)];
    memcpy(ssid, settings.wifi_ssid, sizeof(ssid));
    memcpy(password, settings.wifi_password, sizeof(password));
    memcpy(key, settings.weather_api_key, sizeof(key));

    settings_defaults();
    memcpy(settings.wifi_ssid, ssid, sizeof(settings.wifi_ssid));
    memcpy(settings.wifi_password, password, sizeof(settings.wifi_password));
    memcpy(settings.weather_api_key, key, sizeof(settings.weather_api_key));

    ESP_LOGI(TAG, "settings reset to the defaults, WiFi and API key kept");
    return settings_save();
}
