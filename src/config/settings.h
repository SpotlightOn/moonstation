#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Runtime configuration of the firmware.
//
// config.h only seeds the defaults (see the #ifndef fallbacks in settings.c),
// NVS overrides them at boot and settings_save() writes them back. Modules
// read this struct instead of macros, so a value can change at runtime.
//
// Scope of the web UI: it may write the WiFi credentials, slide directory and
// interval, backlight, clock (deep sleep window and switch, NTP), timezone,
// audio/battery levels, the weather key and position, and its own login -
// everything an owner tunes at home.
// lcd_spi_hz, battery_sample_count, battery_refresh_sec and
// wifi_listen_interval stay config.h only: board level, and a wrong value
// there is not worth a form. Which features are built in stays a compile time
// decision, see features.h.
typedef struct {
    // Slideshow
    uint32_t slide_interval_ms;
    char slide_dir[32];

    // Backlight dimming: levels in percent, delays in seconds
    uint8_t backlight_full_pct;
    uint8_t backlight_dim_pct;
    uint8_t backlight_deep_pct;
    uint8_t backlight_dim_after_sec;
    uint8_t backlight_deep_after_sec;

    // Clock: the window hours apply when CLOCK_DEEP_SLEEP is compiled in
    uint8_t clock_deep_sleep_from; // first local hour of the window
    uint8_t clock_deep_sleep_to;   // first local hour after the window
    int8_t ntp_daily_sync_hour;    // local hour, -1 disables the resync
    char ntp_server[64];

    // WiFi: empty ssid means "not configured", the radio then stays off
    char wifi_ssid[33];
    char wifi_password[65];
    uint16_t wifi_listen_interval; // beacon intervals, 0 = driver default

    // Timezone: standard time minutes east of UTC, EU summer time on top
    int16_t tz_std_offset_min;
    uint8_t tz_eu_dst;

    // Battery
    uint16_t battery_warn_pct;      // low level that beeps, 0 turns it off
    uint16_t battery_refresh_sec;   // minimum age of a cached reading
    uint8_t battery_sample_count;   // ADC samples averaged per reading
    uint8_t battery_charging_active_high; // charge pin high while charging
    uint8_t battery_beep_volume;

    // Audio and display
    uint8_t audio_volume;
    uint32_t lcd_spi_hz;

    // Setup web server: an empty password leaves the page open in the LAN
    char web_user[33];
    char web_password[65];
    uint8_t web_idle_timeout_min;   // silence after which it shuts itself down

    // Night deep sleep of the clock mode: the runtime twin of the
    // CLOCK_DEEP_SLEEP compile switch, so the window can be switched off
    // without a rebuild. Appended, so older blobs still line up field by
    // field and can be migrated.
    uint8_t clock_deep_sleep_enable;

    // Crossfade between two images in milliseconds, 0 cuts hard. Appended
    // last for the same reason; the loader migrates a shorter blob.
    uint16_t slide_fade_ms;

    // Weather mode. Coordinates are kept as strings because they end up in
    // the request URL verbatim; sanitize() restores the build-time default
    // when they do not parse as a coordinate. The key is write-only on the
    // setup page (see webserver.c); an empty one keeps the mode on its "not
    // configured" screen.
    char weather_api_key[73];
    char weather_lat[12];
    char weather_lon[12];
    uint16_t weather_refresh_min; // minutes between two OpenWeather requests
} settings_t;

extern settings_t settings;

// Applies the defaults and overlays what NVS holds. Call once at boot, after
// nvs_flash_init(), before anything reads a value.
esp_err_t settings_init(void);

// Writes the current values to NVS. Call after a change, so a reboot keeps it.
esp_err_t settings_save(void);

// Writes the build-time defaults into *out, without touching NVS or the live
// settings; the setup page shows them as hints next to the labels.
void settings_defaults_into(settings_t *out);

// Reloads the build-time defaults into the live struct without touching NVS.
void settings_defaults(void);

// Stores the build-time defaults again, keeping the WiFi credentials of this
// box: those were typed in here, the config.h value is only a template. This
// is what the setup page's "Reset to default" button calls.
esp_err_t settings_reset_defaults(void);

#endif // SETTINGS_H
