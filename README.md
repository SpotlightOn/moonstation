# Moonstation

ESP-IDF firmware for the Spotpear ESP32-S3 1.28" BOX board (GC9A01 240x240
round display), built with PlatformIO. It runs an endless slideshow of 240x240
JPEG images from an SD card, a clock with date and battery, a weather mode and
an MP3 internet radio, switched by a long press on the touch screen. A 5 s hold
opens a setup page over HTTPS where WiFi, timings, levels, the alarm and the
weather key are changed without a rebuild; a box that has never been on a
network opens its own access point for that page.

![Spotpear Box 1.28](docs/spotpear-box.avif)

## Features

- Endless slideshow of 240x240 baseline JPEGs, sorted by file name, optional
  crossfade between two images; a tap advances immediately
- Clock mode with HH:MM, weekday and date, redrawn only when the minute
  changes, with a small jitter against burn-in. Below it the battery: icon,
  percentage and an arrow while charging. With `CLOCK_DEEP_SLEEP` the display
  stays dark inside the night window and a touch lights it up
- Weather mode (`WEATHER_ENABLE`, on by default): current conditions from
  OpenWeather, a tap shows the next 24 hours, another the next five days, a
  third goes back. Refreshed every 20 minutes and cached in NVS, so the last
  reading survives a reboot without WiFi
- Radio mode (`RADIO_ENABLE`, on by default): MP3 streams from `.pls` playlists
  on the card, one file per station, `http://` only. A drawn button plays and
  pauses, a tap above it is louder and below it quieter, a horizontal swipe
  changes the station; missing playlist, missing WiFi and an unreachable
  station each get their own line and the mode keeps retrying
- Long press (over 0.5 s, released) cycles clock -> slideshow ->
  clock+slideshow -> weather -> radio -> clock (the last two if built in); the
  box boots into clock mode. Only a hold that stays on for 5 s opens the setup
  page, so a quick press never can
- Setup web UI over HTTPS with a login: WiFi, slideshow and radio directories,
  interval, crossfade, backlight, night window and deep sleep, NTP, timezone,
  alarm, audio, battery and weather. Everything is stored in NVS and applied at
  runtime; "Reset to default" restores the firmware defaults, and the page
  footer names the firmware version and build time. A red dot on the lower edge
  shows that it runs, the display shows its address, and it closes itself when
  idle
- First setup without a cable: a box without WiFi opens the access point
  `Moonstation-XXXX` at `192.168.4.1` and serves the page there
- No secrets in the source: WiFi, the page login and the weather key are typed
  in on that page, stored in NVS and never shown again (write-only). The
  factory login `admin` / `admin` has to be replaced on the first visit
- Volume by gesture in every mode: a swipe up is louder, down quieter, one
  5 % step per 40 px of travel, at most five per swipe. A bar shows the level
  for 1.5 s and the level survives a reboot
- Daily alarm at a local time, set on the page: own screen in every mode, two
  rising tones every two seconds, stopped by a tap or after five minutes, and
  it wakes the box from the night deep sleep
- Backlight 100 % while active, dimmed after 15 s, deepest step after 60 s
  inside the night window only, full brightness on any touch. Audio feedback on
  touch and mode changes
- Missing SD card shows a red screen, an empty image directory a yellow one,
  both retried every 2 s; a corrupt JPEG is skipped

## Prerequisites

- PlatformIO (use the bundled venv binary shown below)
- Python 3 with Pillow (only for regenerating the fonts)
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

A new or erased box has no WiFi credentials, so it opens an access point and
serves the setup page there:

1. Power the box. While the access point runs, the display shows `192.168.4.1`
   in the battery row.
2. Join `Moonstation-XXXX` (no password; `XXXX` are the last two bytes of the
   MAC) with a phone or laptop.
3. Open `https://192.168.4.1/` and accept the certificate warning.
4. Log in with `admin` / `admin` and choose your own user and password; the
   factory login stops working as soon as it is saved.
5. Enter your WiFi network and password, plus the weather key and position if
   you use the weather mode, then Save.
6. The box restarts, joins your network and closes the access point. For later
   changes hold a finger on the display for 5 s and open the address it shows.

The access point is open on purpose: it serves nothing but the login form, which
has to be replaced before anything else appears, and it disappears once WiFi is
configured. If the box later cannot reach its network (moved house, new router),
the 5 s gesture falls back to the same access point.

## Setup page

Hold the finger still for 5 s. The box beeps, shows its address in the battery
row and serves `https://<address>/` with a self-signed certificate (the browser
warning on the first visit is expected). The page closes itself after 15 minutes
without a request, or immediately on another 5 s hold.

Everything the owner tunes lives there, applied without a reboot; only WiFi
waits for the next server start (the box restarts itself when it was configured
over its own access point). A changed login takes effect immediately and the
browser asks for the new one right after saving. The password and the weather
key are write-only: the page reports only whether one is stored, an empty field
keeps it, and a checkbox erases the WiFi password or the key.

## Configuration

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
| `ALARM_ENABLED` | `0` | alarm switch at boot (1 = on) |
| `ALARM_HOUR` | `7` | local hour of the alarm (0-23) |
| `ALARM_MINUTE` | `0` | minute of the alarm (0-59) |

### Credentials

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

## SD card

Format the card as FAT32 and place 240x240 baseline JPEGs in the configured
directory; the firmware scans it on boot and loops through the files in name
order. `scripts/build_slides.sh` converts arbitrary images with ImageMagick 7:

```bash
./scripts/build_slides.sh /path/to/images
./scripts/build_slides.sh /path/to/images /path/to/output
```

Then copy the output to the card.

## Hardware

Board: Spotpear ESP32-S3 1.28" BOX, the N16R8 variant that Spotpear also sells
as an AI desktop assistant running Xiaozhi.

- SoC: ESP32-S3, dual-core Xtensa LX7 up to 240 MHz. The firmware runs at
  80 MHz, drops to 10 MHz while both cores are idle and raises the clock to
  240 MHz while the setup page is open
- Flash: 16 MB (W25Q128JVSIQ per Spotpear, reported as 16 MB by the bootloader)
- PSRAM: 8 MB octal at 80 MHz, used for the crossfade frame and the clock-free
  slide copy of combined mode
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

## Development

CI runs cppcheck and a full firmware build on every push to `main` and on pull
requests; pushing a `v*` tag publishes a GitHub Release with the binaries. See
[docs/development.md](docs/development.md) for internals, hardware notes and the
font regeneration.
