# Moonstation

ESP-IDF firmware for the Spotpear ESP32-S3 1.28" BOX board (GC9A01 240x240
round display), built with PlatformIO. It runs an endless slideshow of 240x240
JPEG images from an SD card and offers a clock mode (HH:MM with date) toggled
by a long press on the touch screen. Touch input and the built-in speaker are
supported, and the backlight dims automatically when idle. Holding a finger on
the display for 5 seconds opens a setup page over HTTPS, where WiFi, timings
and levels are changed without rebuilding the firmware.

![Spotpear Box 1.28](docs/spotpear-box.avif)

## Features

- Endless slideshow of 240x240 baseline JPEGs, sorted by file name
- Touch tap advances to the next slide immediately
- **Setup web UI over HTTPS**: hold a finger still on the display for 5 s; a
  short beep confirms that the box took the gesture, then it serves its own
  setup page with a login. It changes WiFi, slideshow, backlight, sleep window,
  NTP, timezone, audio and battery at runtime, and a "Reset to default" button
  restores the firmware defaults (WiFi credentials stay). A red dot on the
  lower edge shows that it runs, the display shows its address, and it shuts
  itself down when idle
- Long press (held over 0.5 s, released) cycles through clock, slideshow and
  combined clock+slideshow modes (boots into clock mode); only a hold that
  stays on for the full 5 s goes to the setup page instead, so a quick press
  can never open it by accident
- Clock mode shows the time and date, with a small jitter for burn-in
  prevention
- Battery indicator below the weekday: icon with the charge level, the
  percentage and an arrow while charging
- Automatic backlight dimming after 15 s and deep dim after 60 s
- Audio feedback on touch and mode changes
- Graceful handling of a missing SD card or empty image directory

## Planned

- alarm clock

## Prerequisites

- PlatformIO (use the bundled venv binary shown below)
- Python 3 with Pillow (for regenerating the clock font)
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

## Configuration

Copy `config.h.example` to `config.h` in the project root and adjust. `config.h`
is git-ignored and must never be committed. The macros below set the defaults:
they are read into runtime settings at boot (`src/config/settings.c`), NVS can
override them, and a firmware without `config.h` behaves exactly like the
fallbacks in that file. Available macros:

| Macro | Default | Meaning |
| --- | --- | --- |
| `SLIDE_INTERVAL_MS` | `10000` | time per slide in milliseconds |
| `SLIDE_DIR` | `/slides` | directory on the SD card |
| `LCD_SPI_HZ` | `40000000` | panel SPI clock |
| `AUDIO_VOLUME` | `60` | speaker volume (0-100) |
| `BACKLIGHT_FULL_PCT` | `100` | brightness while active (percent) |
| `BACKLIGHT_DIM_PCT` | `20` | brightness after `BACKLIGHT_DIM_AFTER_SEC` |
| `BACKLIGHT_DEEP_PCT` | `5` | brightness after `BACKLIGHT_DEEP_AFTER_SEC` |
| `BACKLIGHT_DIM_AFTER_SEC` | `15` | idle seconds until first dim step |
| `BACKLIGHT_DEEP_AFTER_SEC` | `60` | idle seconds until deepest dim step |
| `CLOCK_DEEP_SLEEP` | `0` | builds the night deep sleep in and enables it by default |
| `CLOCK_DEEP_SLEEP_FROM` | `23` | first local hour of the deep sleep window |
| `CLOCK_DEEP_SLEEP_TO` | `7` | first local hour after the deep sleep window |
| `TIMEZONE_STD_OFFSET_HOURS` | `1` | standard (winter) UTC offset for the clock |
| `TIMEZONE_EU_DST` | `1` | observe European summer time (`0` off) |
| `NTP_SERVER` | `pool.ntp.org` | NTP server for the boot, daily and wake-up syncs |
| `NTP_DAILY_SYNC_HOUR` | `4` | local hour of the daily resync while awake (`-1` off) |
| `CLOCK_SHOW_BATTERY` | `1` | battery indicator below the weekday (`0` hides it) |
| `BATTERY_WARN_PCT` | `10` | charge level that turns the indicator red and beeps (`0` off) |
| `BATTERY_BEEP_VOLUME` | `30` | volume of that warning beep in percent |
| `BATTERY_REFRESH_SEC` | `300` | seconds between battery samples |
| `BATTERY_CHARGING_ACTIVE_HIGH` | `1` | charging input reads high while charging |

Optional WiFi settings enable NTP time sync for the clock mode:

```c
// #define WIFI_SSID "your-ssid"
// #define WIFI_PASSWORD "your-password"
// #define WIFI_LISTEN_INTERVAL 10
```

`WIFI_LISTEN_INTERVAL` is the modem-sleep listen interval in beacon intervals
(default 10, about 1 s); it makes maximum modem power saving effective.

The WiFi and NTP stack is always compiled into the firmware; without
`WIFI_SSID` the clock simply keeps the compile time and the radio stays off.
With WiFi configured the firmware connects, waits for the first NTP answer
(up to 10 s) and then switches the radio off again; if no server answers it
keeps the time it had before. It repeats that at `NTP_DAILY_SYNC_HOUR`, since
the RTC drifts by seconds per day without a 32.768 kHz crystal.

With `CLOCK_DEEP_SLEEP` set to `1` the clock mode drops into deep sleep inside
the local hours of the sleep window (default 23:00 to 07:00). The compile
switch only decides whether that code is in the firmware and what the box
starts with; the setup page's *Deep sleep at night* switch turns the same
behaviour off and on at any time. The display
stays dark for the whole window - no minute redraws, no flashes -; a touch
lights it up for the normal dimming period, and the wake-up at the end of the
window brings the clock back at its dimmed level. Every wake-up is a full
reboot: the RTC keeps the time, a touch or the end of the window wakes the
board. The RTC drifts by seconds per day without a 32.768 kHz crystal, so the
wake at the end of the window resyncs via NTP.

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

- ESP32-S3, 16 MB flash, no PSRAM, native USB on GPIO19/20, CPU at 80 MHz
- Panel: GC9A01, 240x240, inversion on, SPI at 40 MHz
- Backlight: GPIO 42, active low, PWM controlled
- SD card: SDMMC 1-bit
- Touch: CST816D on I2C (SDA 11, SCL 7)
- Audio: ES8311 on I2C (SDA 15, SCL 14), I2S, PA on GPIO 46
- Battery: voltage divider on GPIO 1 (ADC1), charging status on GPIO 41

## Runtime behavior

- No SD card: red screen, retry every 2 seconds
- Empty or missing image directory: yellow screen, rescan every 2 seconds
- Unreadable or corrupt JPEG: skipped, next image shown
- Touch advances to the next slide immediately
- Long press cycles clock -> slideshow -> clock+slideshow -> clock; the switch
  redraws immediately
- Clock mode shows HH:MM with the date below, redrawing only once per minute
  (timer driven, no polling)
- Battery readings are sampled by a timer every 5 minutes and shown from that
  cache; when a refresh reports 10 % or less the device beeps quietly

## Development

CI runs cppcheck and a full firmware build on every push to `main` and on pull
requests; pushing a `v*` tag publishes a GitHub Release with the binaries.
See [docs/development.md](docs/development.md) for build details, hardware notes,
configuration, and regeneration of the clock font.