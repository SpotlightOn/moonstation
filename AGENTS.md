# AGENTS.md

Briefing for coding agents working in this repository.

## What this project is

ESP-IDF firmware for the Spotpear ESP32-S3 1.28" BOX board (GC9A01 240x240 round
display), built with PlatformIO. Features: an endless slideshow of 240x240 JPEG
images stored in `/slides` on the SD card, a clock mode (HH:MM and date) toggled
by a long press on the touch screen, PWM backlight dimming, and audio feedback.
Touch and the built-in speaker must be functional.

## Hard rules

1. English only in code, identifiers, comments, serial output, on-screen text and
   commit messages.
2. Never read, edit, grep or cat `config.h` in the project root. It holds local
   settings and is git-ignored. Work with `config.h.example` only and drive every
   new setting through a macro there plus an `#ifndef` fallback in
   `src/config/settings.c`; that file and `src/config/features.h` are the only
   ones that include `config.h`.
   `platformio.ini` adds the project root to the include path (`-I.`).
3. Never commit `config.h`, `*.bin` or `.pio/`.
4. No absolute paths or home directory references in source, comments, docs or
   commit messages.
5. Leave binary and data assets alone.

## Layout

| Path | Purpose |
| --- | --- |
| `platformio.ini` | Single env `spotpear_esp32s3`, espidf framework |
| `partitions_16mb.csv` | 16 MB flash, 4 MB factory app |
| `CMakeLists.txt` | Top-level ESP-IDF project |
| `src/CMakeLists.txt` | Main component registration |
| `src/main.c` | Entry point (`app_main`) |
| `src/config/` | Runtime settings: `config.h` seeds the defaults, NVS overlays (`settings.c/.h`), compile-time switches (`features.h`) |
| `src/core/` | Drivers: `display`, `sdcard`, `touch`, `audio`, `battery`, `net`, `status`, `pins.h` |
| `src/clock/` | Clock mode and time sync (`clock.c/.h`, generated `font.h`) |
| `src/slideshow/` | UI mode manager and slideshow mode (`slideshow.c/.h`) |
| `src/web/` | Setup web server (`webserver.c/.h`); `certs.h` is generated and git-ignored |
| `scripts/generate_font.py` | Regenerates `src/clock/font.h` from the TTF |
| `scripts/generate_cert.sh` | Regenerates `src/web/certs.h`, called by the build |
| `src/idf_component.yml` | Managed ESP-IDF dependencies |
| `config.h.example` | Tracked config template |
| `config.h` | Local settings, git-ignored, off limits |
| `PLAN.md` | Implementation plan |

## Build, flash, monitor

```
~/.platformio/penv/bin/pio run
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/ttyACM0
~/.platformio/penv/bin/pio device monitor
```

Serial capture without the monitor (needs the port open; to catch a boot,
reset the chip over DTR/RTS first, see docs/development.md):

```
stty -F /dev/ttyACM0 115200 raw -echo && timeout 20 cat /dev/ttyACM0
```

The build needs `openssl` on the machine: it generates the self-signed
certificate of the setup page into the git-ignored `src/web/certs.h`.

CI (`.github/workflows/`): `ci.yml` runs cppcheck and `pio run` on pushes to
`main` and on pull requests, building from `config.h.example`, so every new
setting needs an `#ifndef` fallback or CI breaks. `release.yml` publishes a
GitHub Release with the firmware binaries when a `v*` tag is pushed.

## Hardware facts

- ESP32-S3 N16R8, 16 MB flash, 8 MB octal PSRAM (measured: 8 MB at 80 MHz),
  CPU at 80 MHz.
- 8 MB octal PSRAM at 80 MHz is enabled in the build (`CONFIG_SPIRAM` in
  `sdkconfig.defaults`, boot log: `Found 8MB PSRAM device`, SPI SRAM test OK).
  It holds the optional crossfade frame buffer; everything else runs on the
  internal RAM.
- Console: logs go to UART0 on GPIO 43/44 and to the chip's native USB
  Serial/JTAG, which is the `/dev/ttyACM0` on the dev machine. That USB
  peripheral cannot survive light sleep, so keep `PM_LIGHT_SLEEP` off while
  working over it.
- The QMI8658 six-axis IMU on the board is not used.
- Panel: GC9A01, 240x240, inversion on, pins MOSI 2, SCLK 4, CS 5, DC 47,
  RST 38, backlight 42 (active low, PWM). SPI at 40 MHz.
- SD card: SDMMC 1-bit, CLK 17, CMD 18, D0 21, D3 13.
- Touch: CST816D on I2C port 1, SDA 11, SCL 7, RST 6, INT 12, addr 0x15.
- Audio: ES8311 on I2C port 0 (SDA 15, SCL 14), I2S MCLK 16, BCLK 9,
  WS 45, DIN 10, DOUT 8, PA enable 46.

## Conventions

- Work on a `feature/*` branch; never commit to `main` directly.
- One logical change per commit; short English commit message.
- Keep README/AGENTS in sync with build steps, pins and config macros.
- Never commit `config.h` or `.pio/`.
