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

Optional WiFi settings enable NTP time sync for the clock mode.

## Display and backlight

The panel is a GC9A01 driven over SPI. The backlight on GPIO 42 is active low
and PWM controlled (`display_backlight_set_brightness`). `display_backlight_on`
configures the LEDC timer/channel; off sets 0%.

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