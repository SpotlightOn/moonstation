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
│   ├── config/
│   │   ├── features.h
│   │   ├── settings.c
│   │   └── settings.h
│   ├── core/
│   │   ├── display.c/.h
│   │   ├── sdcard.c/.h
│   │   ├── touch.c/.h
│   │   ├── audio.c/.h
│   │   ├── battery.c/.h
│   │   ├── net.c/.h
│   │   ├── status.c/.h
│   │   └── pins.h
│   ├── clock/
│   │   ├── clock.c/.h
│   │   └── font.h
│   ├── slideshow/
│   │   ├── slideshow.c/.h
│   └── web/
│       ├── webserver.c/.h
│       └── certs.h           (generated, git-ignored)
├── scripts/
│   ├── build_slides.sh
│   ├── generate_font.py
│   ├── generate_cert.sh
│   └── DejaVuSans-Bold.ttf
└── docs/
    └── development.md
```

## Prerequisites

- PlatformIO (use the bundled venv binary)
- Python 3 with Pillow (clock font regeneration)
- ImageMagick 7 (`magick`) for preparing slide images
- OpenSSL (the build generates the self-signed certificate of the setup page)

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

That only shows traffic while the port is open, so it misses everything the
board printed before. To capture a boot, open the port and reset the chip
over the DTR/RTS lines of the USB-Serial-JTAG first:

```bash
~/.platformio/penv/bin/python - <<'EOF'
import serial, time
s = serial.Serial("/dev/ttyACM0", 115200, timeout=0.5)
s.dtr = False; s.rts = True;  time.sleep(0.1)
s.dtr = True;  s.rts = False; time.sleep(0.05)
s.dtr = False; s.rts = False
while True:
    print(s.readline().decode(errors="replace"), end="")
EOF
```

## Configuration

Copy `config.h.example` to `config.h` and adjust. `config.h` is git-ignored and
off limits to agents; always edit the example and add `#ifndef` fallbacks in
code. Available macros:

At build time `config.h` seeds the defaults of `src/config/settings.c`, which
is the only file including it. The macros land in `settings_t`, NVS overlays
stored values on top (`settings_init()` at boot, `settings_save()` after a
change), and the modules read the struct. Without NVS content the firmware
behaves exactly like the macros say - the web UI only adds a way to change
the subset of values that makes sense at runtime.

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
| `CLOCK_DEEP_SLEEP` | `0` | deep sleep for the clock mode inside the night window |
| `CLOCK_DEEP_SLEEP_FROM` | `23` | first local hour of the deep sleep window |
| `CLOCK_DEEP_SLEEP_TO` | `7` | first local hour after the deep sleep window |
| `TIMEZONE_STD_OFFSET_HOURS` | `1` | standard (winter) UTC offset for the clock |
| `TIMEZONE_EU_DST` | `1` | observe European summer time (`0` off) |
| `CLOCK_SHOW_BATTERY` | `1` | battery indicator below the weekday (`0` hides it) |
| `BATTERY_WARN_PCT` | `10` | charge level that turns the indicator red and beeps (`0` off) |
| `BATTERY_BEEP_VOLUME` | `30` | volume of that warning beep in percent |
| `BATTERY_REFRESH_SEC` | `300` | seconds between battery samples |
| `BATTERY_CHARGING_ACTIVE_HIGH` | `1` | charging input reads high while charging |

Optional WiFi settings enable NTP time sync for the clock mode.

## Deep sleep in clock mode

`CLOCK_DEEP_SLEEP` makes the clock mode deep sleep inside the local hours of
`CLOCK_DEEP_SLEEP_FROM`/`CLOCK_DEEP_SLEEP_TO` (default 23:00 to 07:00). The
display stays dark for the whole window: the board sleeps until the end of
the window, and a touch lights the clock up for the normal dimming period.
Deep sleep reboots the chip on every wake-up, so the pieces fit together
like this:

- The wake sources are the end of the window (timer) and touch (GPIO 12,
  ext0). The sleep entry refuses to run while the touch line is already low,
  so a held touch cannot loop the boot.
- The sleep check runs before the redraw and the backlight schedule. A
  wake-up that finds the window still active goes straight back to sleep -
  dark from the first millisecond (`main.c` configures the LEDC channel with
  `display_backlight_init()` instead of lighting it up), so no wake-up can
  flash the display at night.
- `display_deep_sleep_prepare()` blanks the backlight, puts the GC9A01 into
  sleep mode and latches the SPI pads (plus the PA pad in `audio_init()`)
  with GPIO holds; without them the pads float and feed noise into the chips
  while the CPU sleeps. There is no PWM in deep sleep, so the backlight can
  only be fully on or off while asleep.
- On the next boot, `display_init()` and `audio_init()` release the holds
  again and reinitialize the panel and the codec from scratch.
- `clock_init()` keeps the RTC time instead of resetting it to the compile
  time and skips WiFi/NTP while the window lasts. The wake at the end of the
  window syncs via NTP again; the RTC has no 32.768 kHz crystal on this board
  and drifts by seconds per day while asleep.
- The window and the displayed time use the local zone from
  `TIMEZONE_STD_OFFSET_HOURS` and `TIMEZONE_EU_DST`, applied as a POSIX TZ
  string in `apply_timezone()` before the compile time is interpreted.

## Display and backlight

The panel is a GC9A01 driven over SPI. The backlight on GPIO 42 is active low
and PWM controlled (`display_backlight_set_brightness`). `display_backlight_init`
configures the LEDC timer/channel and leaves the pad dark; `display_backlight_on`
adds full brightness; off sets 0%.

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
I2C, so the normal path costs only two wake-ups per interaction. Taps and long
presses fire on release: a tap is a hold under 0.5 s, a longer one switches the
mode. Deferring both keeps a deliberate hold free for the setup gesture, where
the finger has to stay at one spot for 5 s. If no release interrupt arrives,
the task falls back to slow 100 ms polling until the finger is lifted. Touch is
optional: missing hardware is logged and ignored.

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
full initial state, so a mode switch is visible right away, not on the next
slide or minute. A long press switches when the finger is lifted.

Adding a new mode (a weather station mode is planned): implement one
`ui_mode_t` struct, link it into the cycle with the `next` field and it is
picked up by the manager automatically.

## Setup mode and status dot

A 5 s hold without drifting (`TOUCH_SETUP_MOVE_PX` in `core/touch.c`) is the
setup gesture: `slideshow.c` calls `web_toggle()`, which starts the setup
server or stops it again. While the server listens, `ui_status_draw()` paints
a red dot in the middle of the lower edge - the round panel cuts off the
corners, so the dot sits a little above the bottom instead of on it - and
`clock.c` shows the address of the page in the row the battery indicator uses.

`core/status.c` exists so the UI only has to read flags: the web server
writes them, clock and slideshow draw them, and neither includes the other.

## Setup web server

`src/web/webserver.c` is deliberately isolated: it talks to `core/net.c` for
the radio, to `settings` for the values and to `status` for what the screen
shows. Nothing else in the firmware knows it exists.

- **Transport**: HTTPS on port 443 (`esp_https_server`). The certificate and
  the key are self-signed and generated at configure time by
  `scripts/generate_cert.sh` into `web/certs.h`, which is git-ignored, so no
  private key lands in the repository. The browser warns about the
  certificate; that is expected for a device without a domain.
- **Sockets**: a TLS socket costs about 40 kB of RAM, so the pool holds two;
  if the heap cannot take the second one, the start retries with one.
- **Auth**: HTTP Basic, checked against a header built once at start. An
  empty `WEB_PASSWORD` switches authentication off and logs a warning.
- **API**: `GET /api/config` returns the editable subset as form encoded
  `key=value` pairs, `POST /api/config` applies and stores it, and
  `POST /api/shutdown` closes the page. `URLSearchParams` on the client side
  reads and writes that format, so quotes, spaces and umlauts in passwords
  survive without an escaping bug on either side.
- **Radio**: the server holds the link with `net_wifi_hold()` for as long as
  it listens, so the daily NTP sync cannot take the page down.
- **Lifetime**: `WEB_IDLE_TIMEOUT_MIN` minutes without a request stop it, so
  a forgotten session does not keep the radio up all night.

## Clock mode

The clock shows HH:MM and the date, with a small per-minute jitter to reduce
burn-in. Time comes from the compile time by default; WiFi/NTP can be enabled
in `config.h`. The radio and the SNTP stack live in `core/net.c` and are
always linked, so firmware built without credentials keeps the compile time
and never touches the radio.

`__DATE__`/`__TIME__` are read as local time: the TZ string is installed
first and `mktime()` resolves summer time, so only the build machine's own
clock error remains in the fallback. With credentials the firmware waits for
the real SNTP answer instead of guessing from a valid-looking system time.

The boot sync is followed by a daily one at `NTP_DAILY_SYNC_HOUR`, armed with
a one-shot `esp_timer` and run in its own task, because the radio/NTP cycle
takes seconds that the timer task must not spend blocked. The WiFi setup
(netif, event loop, driver) runs once per boot, only start/connect/stop
repeat, so a second station interface can never be created.

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

## Continuous integration

`.github/workflows/ci.yml` runs on pushes to `main` and on pull requests:

- **Static analysis**: cppcheck over `src` (warnings, performance and
  portability checks, any finding fails the run).
- **Build firmware**: `pio run` on a clean runner with `config.h` created from
  `config.h.example`, the PlatformIO toolchain cached. `firmware.bin`,
  `bootloader.bin` and `partition-table.bin` are uploaded as a workflow
  artifact.

`.github/workflows/release.yml` runs on version tags (`v*`), builds the same
way, merges the images into a single flashable binary and publishes a GitHub
Release with all binaries attached:

```bash
git tag v0.1.0
git push origin v0.1.0
```

## Debugging

Use the serial monitor at 115200 baud. Build artifacts and logs are under
`.pio/`. Check init results for display, SD card, touch and audio on boot.

## Conventions

- One logical change per commit; short English commit messages
- Keep `README.md`, `AGENTS.md` and `PLAN.md` in sync with the code
- Never commit `config.h`, `*.bin` or `.pio/`
- Leave binary and data assets (including slide images) untouched