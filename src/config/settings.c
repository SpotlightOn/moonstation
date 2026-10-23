#include "settings.h"
#include <stdio.h>
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

#define TZ_STD_OFFSET_MIN \
    ((int16_t)(TIMEZONE_STD_OFFSET_HOURS * 60 + \
               (TIMEZONE_STD_OFFSET_HOURS >= 0 ? 0.5 : -0.5)))

static const char *TAG = "settings";
static const char *NVS_NAMESPACE = "settings";
static const char *NVS_KEY = "config";

// Stored as one versioned blob: bump it whenever the struct layout changes,
// so an old blob falls back to the defaults instead of reading garbage. The
// loader still takes the version before that, as long as nothing but appended
// fields changed - see settings_init().
#define SETTINGS_VERSION 3

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
    };
    *out = d;
}

void settings_defaults(void)
{
    settings_defaults_into(&settings);
}

// Keeps a broken or hand edited store from taking the firmware down: values
// are clamped to what the code can work with.
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
        if (err == ESP_OK && len == sizeof(blob) &&
            (blob.version == SETTINGS_VERSION || blob.version == SETTINGS_VERSION - 1)) {
            settings = blob.values;
            if (blob.version == SETTINGS_VERSION - 1) {
                // Version 2 ended before the deep sleep switch, so that member
                // holds padding bytes of the old blob; it takes the default.
                settings.clock_deep_sleep_enable = CLOCK_DEEP_SLEEP;
            }
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
    // The WiFi credentials belong to this box and were entered by hand, the
    // build time value for them is a template at best. Everything else goes
    // back to the configuration the firmware was built with.
    char ssid[sizeof(settings.wifi_ssid)];
    char password[sizeof(settings.wifi_password)];
    memcpy(ssid, settings.wifi_ssid, sizeof(ssid));
    memcpy(password, settings.wifi_password, sizeof(password));

    settings_defaults();
    memcpy(settings.wifi_ssid, ssid, sizeof(settings.wifi_ssid));
    memcpy(settings.wifi_password, password, sizeof(settings.wifi_password));

    ESP_LOGI(TAG, "settings reset to the defaults, WiFi kept");
    return settings_save();
}
