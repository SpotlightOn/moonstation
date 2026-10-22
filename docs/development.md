# Development

Developer notes for the moonstation firmware. See `README.md` for the user-facing
overview.

## Layout

```
moonstation/
├── platformio.ini
├── partitions_16mb.csv
├── config.h.example
├── config.h                 (git-ignored, local only)
├── src/
│   ├── main.c
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   ├── core/
│   │   ├── display.c/.h
│   │   ├── sdcard.c/.h
│   │   ├── touch.c/.h
│   │   ├── audio.c/.h
│   │   ├── battery.c/.h
│   │   └── pins.h
│   ├── clock/
│   │   ├── clock.c/.h
│   │   └── font.h
│   └── slideshow/
│       ├── slideshow.c/.h
├── scripts/
│   ├── build_slides.sh
│   ├── generate_font.py
│   └── DejaVuSans-Bold.ttf
└── docs/
    └── development.md
```

## Prerequisites

- PlatformIO (use the bundled venv binary)
- Python 3 with Pillow (clock font regeneration)
- ImageMagick 7 (`magick`) for preparing slide images

## Build, flash, monitor

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

Copy `config.h.example` to `config.h` and adjust. `config.h` is git-ignored and
off limits to agents; always edit the example and add `#ifndef` fallbacks in
code. Available macros:

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
| `CLOCK_SHOW_BATTERY` | `1` | battery indicator below the weekday (`0` hides it) |
| `BATTERY_WARN_PCT` | `10` | charge level that turns the indicator red and beeps (`0` off) |
| `BATTERY_BEEP_VOLUME` | `30` | volume of that warning beep in percent |
| `BATTERY_REFRESH_SEC` | `300` | seconds between battery samples |
| `BATTERY_CHARGING_ACTIVE_HIGH` | `1` | charging input reads high while charging |

Optional WiFi settings enable NTP time sync for the clock mode.

## Display and backlight

The panel is a GC9A01 driven over SPI. The backlight on GPIO 42 is active low
and PWM controlled (`display_backlight_set_brightness`). `display_backlight_on`
configures the LEDC timer/channel; off sets 0%.

Colors in the code are plain RGB565; the byte swap for the panel happens
exactly once per write path with `RGB565_SWAP()` from `display.h`: in
`fill_rect()`, `draw_text()` and `draw_time_text()` (clock.c), in `fill_color()`
(slideshow.c) and in the JPEG decoder (`swap_color_bytes = 1`). The swap is
needed because the panel expects the most significant byte first, esp_lcd
reverses only command and parameter bytes (not color data), and the CPU stores
a `uint16_t` least significant byte first. White and black are palindromic and
therefore unaffected, which is why a missing swap only shows up as blue instead
of red and dark red instead of green. Never put pixels into a frame buffer
without going through one of those paths.

## SD card and images

The card is mounted over SDMMC (1-bit). Images must be 240x240 baseline JPEGs.
Use `scripts/build_slides.sh` to convert arbitrary images, then copy the output
to the SD card.

## Touch

CST816D on I2C port 1. Touch is interrupt driven: the controller's INT line
(GPIO 12, falling edge) wakes a dedicated task that reads the controller over
I2C, so the normal path costs only two wake-ups per interaction. Taps fire on
release; a long press (about 0.5 s) fires as soon as the hold crosses the
threshold, without waiting for the release. If no release interrupt arrives
after that, the task falls back to slow 100 ms polling until the finger is
lifted. Touch is optional: missing hardware is logged and ignored.

## Audio

ES8311 on I2C port 0 plus I2S. The codec is opened per tone and closed
afterwards so the PA stays off when idle. `audio_play_tone(freq, ms)` generates
a sine wave.

## Battery

`core/battery.c` reads the charge level from the voltage divider on GPIO 1
(ADC1 channel 0, 12 dB attenuation, 12-bit) and the charging input on GPIO 41
(internal pull-up, high while charging). The raw readings are averaged over
three samples and mapped to 0-100 % with a per-board lookup table taken from
the vendor firmware, so no ADC calibration config is needed.

A repeating `esp_timer` samples the battery every `BATTERY_REFRESH_SEC`
seconds and writes one log line per refresh containing the raw ADC value, so
a wrong percentage can be traced back to the sensor. The display only reads
that cache; it never starts a conversion itself. When a refresh reports
`BATTERY_WARN_PCT` or less, a short quiet beep sounds (`BATTERY_BEEP_VOLUME`).
The monitor is initialized from `clock_init()`.

## UI modes

The main loop in `slideshow.c` is a mode manager. Every mode implements the
`ui_mode_t` interface (the shared "base class" of callbacks): `enter`/`exit`
for mode transitions, `on_tap`/`on_long_press` for touch events (run in the
touch task), `on_wake` for periodic work, and `next_wake_us` telling the
manager when the mode wants to be woken next.

The modes are `clock`, `slideshow` and `combined` (clock + slideshow). A long
press cycles through them in this order: **clock -> slideshow -> combined ->
clock**. The device boots into clock mode. `enter()` always redraws the
full initial state, so a mode switch is visible immediately (a long press
performs the switch right away, not on the next slide or minute).

Adding a new mode (a weather station mode is planned): implement one
`ui_mode_t` struct, link it into the cycle with the `next` field and it is
picked up by the manager automatically.

## Clock mode

The clock shows HH:MM and the date, with a small per-minute jitter to reduce
burn-in. Time comes from the compile time by default; WiFi/NTP can be enabled
in `config.h`.

Below the weekday sits the battery row: a small battery icon filled to the
charge level, the percentage next to it, and an upward arrow while charging.
It is white normally, green while charging and red at or below
`BATTERY_WARN_PCT`, and it moves with the same jitter as the date. Set
`CLOCK_SHOW_BATTERY` to `0` to hide it.

The clock is timer driven: the screen is redrawn only when the displayed minute
changes, aligned to the minute boundary. A one-shot `esp_timer` (`ui_wake`)
wakes the main loop at the next slide, minute boundary or backlight step, and
touch callbacks wake it via a task notification, so there is no polling redraw.
In combined mode the overlay is refreshed the same way when the minute changes.

## Clock font

The clock renders text with a bitmap font stored in `src/clock/font.h`. To
regenerate it from `scripts/DejaVuSans-Bold.ttf` (requires Pillow):

```bash
python3 scripts/generate_font.py
```

Commit the resulting `src/clock/font.h` when the font changes.

## Debugging

Use the serial monitor at 115200 baud. Build artifacts and logs are under
`.pio/`. Check init results for display, SD card, touch and audio on boot.

## Conventions

- One logical change per commit; short English commit messages
- Keep `README.md`, `AGENTS.md` and `PLAN.md` in sync with the code
- Never commit `config.h`, `*.bin` or `.pio/`
- Leave binary and data assets (including slide images) untouched