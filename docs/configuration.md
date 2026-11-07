# Configuration

Copy `config.h.example` to `config.h` in the project root and adjust it. The file
is tracked, so it must not contain a credential. The macros are the defaults:
they are read into runtime settings at boot (`src/config/settings.c`), NVS can
override them, and a firmware without `config.h` behaves like the fallbacks in
that file.

| Macro | Default | Meaning |
| --- | --- | --- |
| `SLIDE_INTERVAL_MS` | `10000` | time per slide in milliseconds |
| `SLIDE_DIR` | `/slides` | directory on the SD card |
| `SLIDE_FADE_MS` | `0` | crossfade between two images in ms (0 = hard cut) |
| `SLIDE_PM_LOW_POWER` | `1` | release the CPU speed lock between two images |
| `PM_LIGHT_SLEEP` | `0` | automatic light sleep (drops the USB console) |
| `PM_MIN_CPU_MHZ` | `10` | idle CPU frequency floor (`40` to stay conservative) |
| `LCD_SPI_HZ` | `40000000` | panel SPI clock |
| `AUDIO_VOLUME` | `60` | speaker volume (0-100) |
| `BACKLIGHT_FULL_PCT` | `100` | brightness while active (percent) |
| `BACKLIGHT_DIM_PCT` | `20` | brightness after `BACKLIGHT_DIM_AFTER_SEC` |
| `BACKLIGHT_DEEP_PCT` | `5` | brightness after `BACKLIGHT_DEEP_AFTER_SEC`, night window only |
| `BACKLIGHT_DIM_AFTER_SEC` | `15` | idle seconds until first dim step |
| `BACKLIGHT_DEEP_AFTER_SEC` | `60` | idle seconds until deepest dim step (night window only) |
| `CLOCK_DEEP_SLEEP` | `0` | builds the night deep sleep in and enables it by default |
| `CLOCK_DEEP_SLEEP_FROM` | `23` | first local hour of the deep sleep window |
| `CLOCK_DEEP_SLEEP_TO` | `7` | first local hour after the deep sleep window |
| `TIMEZONE_STD_OFFSET_HOURS` | `1` | standard (winter) UTC offset for the clock |
| `TIMEZONE_EU_DST` | `1` | observe European summer time (`0` off) |
| `NTP_SERVER` | `pool.ntp.org` | NTP server for the boot, daily and wake-up syncs |
| `NTP_DAILY_SYNC_HOUR` | `4` | local hour of the daily resync while awake (`-1` off) |
| `CLOCK_SHOW_BATTERY` | `1` | battery indicator below the date (`0` hides it) |
| `BATTERY_WARN_PCT` | `10` | charge level that turns the indicator red and beeps (`0` off) |
| `BATTERY_BEEP_VOLUME` | `30` | volume of that warning beep in percent |
| `BATTERY_REFRESH_SEC` | `300` | seconds between battery samples |
| `BATTERY_CHARGING_ACTIVE_HIGH` | `1` | charging input reads high while charging |
| `WIFI_LISTEN_INTERVAL` | `10` | modem-sleep listen interval in beacon intervals |
| `WEB_IDLE_TIMEOUT_MIN` | `15` | minutes without a request before the page closes |
| `WEATHER_LAT` | `52.5200` | latitude the forecast is fetched for |
| `WEATHER_LON` | `13.4050` | longitude the forecast is fetched for |
| `WEATHER_REFRESH_MIN` | `20` | minutes between two OpenWeather refreshes (5-1440) |
| `RADIO_DIR` | `/radio` | folder with the `.pls` playlists of the radio mode |
| `MOVIE_DIR` | `/movies` | folder with the movies of the movie mode |
| `ALARM_ENABLED` | `0` | alarm switch at boot (1 = on) |
| `ALARM_HOUR` | `7` | local hour of the alarm (0-23) |
| `ALARM_MINUTE` | `0` | minute of the alarm (0-59) |

## Credentials

WiFi, the login of the setup page and the OpenWeather key are not part of the
template: they are typed in on the page and stored in NVS, and "Reset to
default" keeps them. `config.h` can be committed - check `git diff` for
credentials before you do. `WIFI_SSID` and `WIFI_PASSWORD` still compile in as an
emergency hatch, both empty and commented out in the template, for the case
where the access point cannot be used.

The WiFi and NTP stack is always linked. Without an SSID the clock keeps the
compile time and only the setup access point runs; with WiFi the firmware
connects, waits up to 10 s for the first NTP answer and switches the radio off
again, repeating that at `NTP_DAILY_SYNC_HOUR` because the RTC drifts by seconds
per day without a 32.768 kHz crystal.

The weather mode requests by coordinate in metric units, two calls per refresh;
a failed refresh is retried after 5 minutes, and a reading older than two
intervals gets a small amber ring at the top of the screen.

With `CLOCK_DEEP_SLEEP` the clock mode deep sleeps inside the night window
(default 23:00 to 07:00); the setup page's *Deep sleep at night* switch turns
the same behaviour off and on at any time. The display stays dark for the whole
window, a touch lights it up for the normal dimming period, and a pending alarm
breaks the sleep at its time, rings, and sends the box back afterwards. Every
wake-up is a full reboot: the RTC keeps the time, and the wake at the end of the
window resyncs via NTP.
