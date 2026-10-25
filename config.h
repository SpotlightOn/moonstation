// Local build configuration of this box.
//
// The file is tracked in git and must never hold a secret: WiFi, the login of
// the setup page and the weather key are typed in on that page and live in
// NVS. See config.h.example for the documented template.

#define CONFIG_H

#define CLOCK_DEEP_SLEEP 1

// ================================== Debug ==================================

// Write the frame after every mode switch as a 240x240 BMP to
// /sdcard/screens, so a layout problem can be checked without a photo of the
// panel. Turn it off again after debugging: the UI waits for the card while
// the file is written.
#define SCREENSHOT_ENABLE 0

// Additionally print each frame as hex on the console. Then a serial capture
// is enough to rebuild the image, no card reader needed. Costs about 250 kB of
// console output per frame and needs SCREENSHOT_ENABLE.
#define SCREENSHOT_SERIAL 0

// ---------------- Slideshow ----------------
#define SLIDE_INTERVAL_MS 10000
#define SLIDE_DIR "/slides"

// ============================ Setup web server =============================

// Login of the configuration page: set on the page itself and stored in NVS,
// so it is deliberately not part of this file.

// Minutes without a request before the server shuts itself down again.
#define WEB_IDLE_TIMEOUT_MIN 15

// ---------------- Weather ----------------

// Position the forecast is fetched for, in decimal degrees (WGS84). The
// weather mode asks OpenWeather directly by coordinate; the city name query
// is deprecated. Both values can also be changed on the setup page.
#define WEATHER_LAT 52.5200
#define WEATHER_LON 13.4050

// Minutes between two OpenWeather requests (5..1440). One refresh costs two
// requests, which leaves the free tier of 1000 calls per day untouched even
// at the shortest interval.
#define WEATHER_REFRESH_MIN 20

// 1 = detect the city from the public IP address (ip-api.com)
// 0 = use CITY_CODE below
#define CITY_AUTO_DETECT 1

// City name for OpenWeatherMap when CITY_AUTO_DETECT is 0, for example
// "Berlin,de" or "Shanghai,cn". The numeric ids in CityCode/citycode.json
// belong to a Chinese weather service and do not work with the q= query this
// sketch sends.
#define CITY_CODE "101020100"

// ---------------- Time ----------------
// NTP server the clock syncs against
#define NTP_SERVER "time.nist.gov"
// Resync interval in minutes
#define NTP_SYNC_MINUTES 20

// ---------------- Time zone ----------------
// 0 = use the UTC offset that OpenWeatherMap reports for the detected city.
//     Correct while the weather request succeeds, but the clock falls back to
//     UTC as soon as that request fails (expired or rate limited API key).
// 1 = use the fixed standard offset below plus the European summer time rule.
#define TIMEZONE_SOURCE 1

// Standard (winter) UTC offset in hours, decimals allowed.
// Germany = 1 (CET). Summer time adds one hour on top.
#define TIMEZONE_STD_OFFSET_HOURS 1

// 1 = observe European summer time (last Sunday in March to the last Sunday in
//     October), 0 = keep the standard offset all year
#define TIMEZONE_EU_DST 1

// ---------------- Display ----------------
#define LCD_SPI_HZ 40000000
// Battery indicator below the weekday in the clock (1 = on, 0 = off)
// #define CLOCK_SHOW_BATTERY 1
// Charge level (percent) at or below which the indicator turns red and the
// device beeps quietly on every refresh (0 = no warning)
// #define BATTERY_WARN_PCT 10
// Volume of that warning beep in percent (0-100)
// #define BATTERY_BEEP_VOLUME 30
// Seconds between battery samples (the ADC is not read more often than this)
// #define BATTERY_REFRESH_SEC 300
// Polarity of the charging input: 1 = the pin reads high while charging
// #define BATTERY_CHARGING_ACTIVE_HIGH 1
// Backlight auto-dimming (levels in percent, delays in seconds)
// #define BACKLIGHT_FULL_PCT 100
// #define BACKLIGHT_DIM_PCT 20
// #define BACKLIGHT_DEEP_PCT 5
// #define BACKLIGHT_DIM_AFTER_SEC 15
// #define BACKLIGHT_DEEP_AFTER_SEC 60

// ---------------- Audio ----------------
#define AUDIO_VOLUME 60
