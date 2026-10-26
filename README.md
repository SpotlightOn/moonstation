# Moonstation

ESP-IDF firmware for the Spotpear ESP32-S3 1.28" BOX board (GC9A01 240x240
round display), built with PlatformIO. It runs an endless slideshow of 240x240
JPEG images from an SD card and offers a clock mode (HH:MM with date) toggled
by a long press on the touch screen. An optional weather mode shows the current
conditions from OpenWeather and, on a tap, the next 24 hours and then the next
five days, and a daily alarm rings at a time set on the setup page. Touch input
and the built-in speaker are supported, and the backlight dims automatically
when idle.
Holding a finger on the display for 5 seconds opens a setup page over HTTPS,
where WiFi, timings, levels, the alarm and the weather key are changed without
rebuilding the firmware. A box that has never been on a network
opens its own access point and serves that page there.

![Spotpear Box 1.28](docs/spotpear-box.avif)

## Features

- Endless slideshow of 240x240 baseline JPEGs, sorted by file name, with an
  optional crossfade between two images
- Touch tap advances to the next slide immediately
- **Setup web UI over HTTPS**: hold a finger still on the display for 5 s; a
  short beep confirms that the box took the gesture, then it serves its own
  setup page with a login. It changes WiFi, slideshow, backlight, sleep window,
  NTP, timezone, alarm, audio, battery and the weather key and position at
  runtime, and a "Reset to default" button restores the firmware defaults. A
  red dot on the lower edge shows that it runs, the display shows its address,
  and it shuts itself down when idle
- **First setup without a cable**: a box without WiFi opens its own access
  point (`Moonstation-XXXX`, address `192.168.4.1`), so the page is reachable
  from a phone before the box has ever been on a network
- No secrets in the source: WiFi, the page login and the weather key are typed
  in on that page, stored in NVS and never shown again (write-only fields). The
  factory login `admin` / `admin` must be replaced on the first visit
- Long press (held over 0.5 s, released) cycles through clock, slideshow,
  combined clock+slideshow, weather and radio modes (the last two if built
  in); it boots into clock mode. Only a hold that stays on for the full 5 s
  goes to the setup page instead, so a quick press can never open it by
  accident
- Volume by gesture in every mode: a swipe up is louder, a swipe down quieter
  in 5 % steps - one step per 40 px of travel, at most five per swipe. A bar
  shows the new level for a second and a half. The level is stored, so it
  survives a reboot
- Daily alarm at a local time, set on the setup page. It beeps and shows its
  own screen in every mode, stops on a tap and by itself after five minutes; a
  pending alarm also wakes the box from its night deep sleep
- Weather mode (`WEATHER_ENABLE` in `src/config/features.h`, on by default):
  current conditions from OpenWeather, a tap shows the next 24 hours in
  3-hour steps, another tap the next five days, a third goes back to now.
  Refreshed every 20 minutes by default and cached in NVS, so the mode still
  shows the last reading after a reboot without WiFi. Without a key it shows a
  hint instead
- Radio mode (`RADIO_ENABLE` in `src/config/features.h`, on by default): plays
  MP3 internet radio from `.pls` playlists on the SD card, one file per
  station. A drawn button in the middle of the screen plays and pauses, a tap
  above it is louder and below it quieter, a horizontal swipe changes the
  station, and sliding still sets the volume as in every mode - so a tap that
  was meant as a slide never stops the station. Missing playlists, missing WiFi
  or an unreachable station each get their own line on screen; the mode keeps
  retrying in the background. Plain `http://` only, no TLS, and nothing plays
  once the long press leaves the mode
- Clock mode shows the time, the weekday and the date, with a small jitter for
  burn-in prevention
- Battery indicator below the date: icon with the charge level, the percentage
  and an arrow while charging
- Automatic backlight dimming after 15 s; the deepest level is reached inside
  the night window only, so the clock never goes dark during the day
- Audio feedback on touch and mode changes
- Graceful handling of a missing SD card or empty image directory

## Prerequisites

- PlatformIO (use the bundled venv binary shown below)
- Python 3 with Pillow (for regenerating the fonts)
- ImageMagick 7 (`magick`) for preparing slide images
- OpenSSL (the build generates the self-signed certificate of the setup page)

## Build, flash, monitor

`pio` is usually not on `PATH`; use the PlatformIO venv binary:

```bash
~/.platformio/penv/bin/pio run
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/ttyACM0
~/.platformio/penv/bin/pio device monitor
```

Serial capture without the monitor:

```bash
stty -F /dev/ttyACM0 115200 raw -echo && timeout 20 cat /dev/ttyACM0
```

## First setup

A new or erased box has no WiFi credentials, so there is no network to reach
the page from. It solves that itself: on the first boot it opens an access
point and serves the setup page there.

1. Power the box. While the access point runs, the display shows `192.168.4.1`
   in the battery row.
2. Join the WiFi network `Moonstation-XXXX` with a phone or laptop. `XXXX` are
   the last two bytes of the box's MAC, and the network has no password.
3. Open `https://192.168.4.1/` and accept the certificate warning.
4. Log in with the factory login `admin` / `admin`. The page shows nothing but
   one form: choose your own user and password. The factory login stops working
   as soon as it is saved.
5. The settings page opens. Enter your WiFi network name and password, plus the
   weather key and position if you use the weather mode, then Save.
6. The box restarts, joins your network and closes the access point. For later
   changes hold a finger on the display for 5 s and open the address it shows.

The access point is open on purpose: it serves nothing but the login form, the
factory login has to be replaced before anything else appears, and the AP
disappears once WiFi is configured. If the box later cannot reach its network
(moved house, new router), the 5 s gesture falls back to the same access point.

## Setup page

Hold the finger still on the display for 5 seconds. The box beeps, shows its
address in the battery row and serves `https://<address>/` with a self-signed
certificate (the browser warning on the first visit is expected). The page
closes itself after 15 minutes without a request, or immediately on another
5 s hold.

Everything the owner tunes lives there: WiFi credentials, the login, the
weather key and position, slideshow and radio directories, interval,
crossfade, backlight levels and delays, night window and deep sleep, NTP and
timezone, daily alarm, audio volume, battery warning level and the idle
timeout. All of
it is stored in NVS and applied without a reboot; only WiFi waits for the next
server start
(the box restarts itself when it was configured over its own access point).

The login is never empty. A changed user or password takes effect immediately
and the browser asks for the new one right after saving. The password and the
weather key are write-only: the page reports only whether one is stored, an
empty field keeps it, and a checkbox erases the WiFi password or the key.

## Configuration

Copy `config.h.example` to `config.h` in the project root and adjust it. The
file is tracked in git, so it must not contain a secret - see the note on
credentials below. The macros set the defaults: they are read into runtime
settings at boot (`src/config/settings.c`), NVS can override them, and a
firmware without `config.h` behaves exactly like the fallbacks in that file.

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
| `ALARM_ENABLED` | `0` | alarm switch at boot (1 = on) |
| `ALARM_HOUR` | `7` | local hour of the alarm (0-23) |
| `ALARM_MINUTE` | `0` | minute of the alarm (0-59) |

### Credentials

WiFi, the login of the setup page and the OpenWeather key are not part of the
template: they are typed in on the page and stored in NVS.

- A box without WiFi opens its own access point on the first boot, see
  [First setup](#first-setup). Neither a cable nor a rebuild is needed.
- The login ships as `admin` / `admin` and has to be replaced before the page
  shows anything else. It lives in NVS too, so "Reset to default" keeps it.
- `config.h` can be committed. Run `git diff` before you do and make sure no
  credential sneaked in. `WIFI_SSID` and `WIFI_PASSWORD` still compile in as an
  emergency hatch - both empty by default and commented out in the template -
  for the case where the access point cannot be used.

The WiFi and NTP stack is always compiled in; without an SSID the clock keeps
the compile time and the radio runs the setup access point on the first boot.
With WiFi configured the firmware connects, waits for the first NTP answer (up
to 10 s) and switches the radio off again; if no server answers it keeps the
time it had before. It repeats that at `NTP_DAILY_SYNC_HOUR`, since the RTC
drifts by seconds per day without a 32.768 kHz crystal.

`WEATHER_ENABLE` in `src/config/features.h` (`0` removes the mode from the
cycle) builds the weather mode in. The request is made by coordinate in metric
units, two requests per refresh. A refresh that fails is retried after
5 minutes, and a reading older than two intervals gets a small amber ring at
the top of the screen.

With `CLOCK_DEEP_SLEEP` set to `1` the clock mode drops into deep sleep inside
the local hours of the sleep window (default 23:00 to 07:00). The compile
switch only decides whether that code is in the firmware and what the box
starts with; the setup page's *Deep sleep at night* switch turns the same
behaviour off and on at any time. The display stays dark for the whole window -
no minute redraws, no flashes - and a touch lights it up for the normal
dimming period. A pending alarm breaks the sleep at its time and rings, so an
alarm inside the window is not lost; afterwards the box goes back to sleep. The
wake-up at the end of the window brings the clock back at full brightness and
starts the normal dimming schedule. Every wake-up is a full
reboot: the RTC keeps the time, and with WiFi configured the wake at the end of
the window resyncs via NTP.

## SD card

Format the card as FAT32 and place 240x240 baseline JPEGs in the configured
directory. The firmware scans it on boot and loops through the files in name
order.

### Preparing images

A helper script converts arbitrary JPEGs/PNGs to the required format using
ImageMagick 7:

```bash
./scripts/build_slides.sh /path/to/images
./scripts/build_slides.sh /path/to/images /path/to/output
```

Then copy the output to the SD card.

## Hardware

Board: Spotpear ESP32-S3 1.28" BOX, the N16R8 variant that Spotpear also sells
as an AI desktop assistant running Xiaozhi.

- SoC: ESP32-S3, dual-core Xtensa LX7 up to 240 MHz. The firmware runs at
  80 MHz, drops to 10 MHz while both cores are idle and raises the clock to
  240 MHz while the setup page is open
- Flash: 16 MB (W25Q128JVSIQ per Spotpear, reported as 16 MB by the bootloader)
- PSRAM: 8 MB octal at 80 MHz, enabled in the build and used for the crossfade
  frame and the clock-free slide copy of combined mode
- Panel: GC9A01, 1.28", 240x240, 65K colours, inversion on, SPI at 40 MHz
  (MOSI 2, SCLK 4, CS 5, DC 47, RST 38)
- Backlight: GPIO 42, active low, PWM controlled
- SD card: SDMMC 1-bit (CLK 17, CMD 18, D0 21, D3 13)
- Touch: CST816D on I2C (SDA 11, SCL 7, INT 12, RST 6, address 0x15)
- Audio: ES8311 codec on I2C (SDA 15, SCL 14) with I2S (MCLK 16, BCLK 9,
  WS 45, DIN 10, DOUT 8), PA on GPIO 46
- Battery: 3.7 V lithium connector with ETA6096 charger, voltage divider on
  GPIO 1 (ADC1), charging status on GPIO 41
- Motion: QMI8658 six-axis IMU (accelerometer and gyroscope) per Spotpear, not
  used by this firmware
- Console: logs go to UART0 (GPIO 43/44) and to the chip's native USB
  Serial/JTAG, which is the `/dev/ttyACM0` on a Linux host

## Runtime behavior

- No WiFi configured: the clock keeps the compile time and the box opens its
  setup access point on the first boot
- No SD card: red screen, retry every 2 seconds
- Empty or missing image directory: yellow screen, rescan every 2 seconds
- Unreadable or corrupt JPEG: skipped, next image shown
- Touch advances to the next slide immediately
- A vertical swipe anywhere changes the volume, one 5 % step per 40 px of
  travel (at most five), shown as a bar in the middle of the screen for 1.5 s;
  a horizontal swipe belongs to the mode and never counts as a tap
- In the radio mode the drawn button in the middle of the screen plays and
  pauses, a tap above it is louder and below it quieter, and a horizontal swipe
  changes the station
- Long press cycles clock -> slideshow -> clock+slideshow -> weather -> radio
  (the last two if built in) -> clock; the switch redraws immediately
- Weather mode taps flip between current conditions and the 24-hour forecast;
  the long press still changes the mode
- The alarm rings once a day at its local time with two rising tones every two
  seconds and a blinking `ALARM` screen; a tap or a long press stops it, and so
  does a five minute safety timeout
- Clock mode shows HH:MM with the weekday and date below, redrawing only once
  per minute (timer driven, no polling)
- Backlight: 100 % while active, `BACKLIGHT_DIM_PCT` after 15 s, the deepest
  level after 60 s inside the night window only, full brightness on any touch
- Battery readings are sampled by a timer every 5 minutes and shown from that
  cache; when a refresh reports 10 % or less the device beeps quietly

## Development

CI runs cppcheck and a full firmware build on every push to `main` and on pull
requests; pushing a `v*` tag publishes a GitHub Release with the binaries.
See [docs/development.md](docs/development.md) for build details, hardware notes,
configuration, and regeneration of the fonts.
