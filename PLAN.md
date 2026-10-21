# Plan: moonstation firmware (ESP-IDF, PlatformIO)

Goal: Build an ESP-IDF firmware for the Spotpear ESP32-S3 1.28" BOX board,
initially set up as a PlatformIO project. It must support the same hardware as
the vendor Arduino projects (display, SD card, touch, built-in speaker) and show
an endless slideshow of 240x240 JPEG images from `/slides` on the SD card. The
slide interval is configurable in `config.h` and defaults to 5 seconds.

## 1. Hardware target (from the reference projects)

Board: Spotpear ESP32-S3 1.28" BOX, ESP32-S3, 16 MB flash, no PSRAM, native USB.

| Function | Pins |
| --- | --- |
| Display SPI (GC9A01, 240x240) | SCLK 4, MOSI 2, CS 5, DC 47, RST 38, backlight 42 (active low) |
| SPI clock | 40 MHz |
| SD card (SDMMC, 1-bit) | CLK 17, CMD 18, D0 21, D3/CS 13 |
| Touch (CST816D, I2C) | SDA 11, SCL 7, RST 6, INT 12, addr 0x15 |
| Audio codec (ES8311) | I2C SDA 15, SCL 14, addr default; I2S MCLK 16, BCLK 9, WS 45, DIN 10, DOUT 8; PA enable 46 |
| LED | GPIO 48 |
| Boot button | GPIO 0 |
| Battery | ADC GPIO 1, charging GPIO 41 |

Source of truth for pins: `main/boards/spotpear/sp-esp32-s3-1.28-box/config.h`
in the xiaozhi-esp32 reference and `src/main.cpp` in ESP32S3-SDcard-Video.

## 2. PlatformIO setup

- `platformio.ini` with `framework = espidf`, `board = esp32-s3-devkitc-1`.
- Flash: 16 MB, `board_build.partitions = partitions_16mb.csv`, 4 MB app slot.
- CPU 240 MHz, USB CDC on boot (`-DARDUINO_USB_CDC_ON_BOOT` is Arduino-only;
  for ESP-IDF use the USB serial JTAG / CDC console as configured by the framework).
- `board_build.flash_mode = qio`, `board_build.flash_size = 16MB`.
- Include path `-I.` so `#include "config.h"` resolves from the sketch/app dir.
- Managed components via `idf_component.yml` (not Arduino `lib_deps`).

## 3. Dependencies (ESP-IDF managed components)

- `espressif/esp_lcd_gc9a01` - panel driver.
- `espressif/esp_lcd_touch` + `espressif/esp_lcd_touch_cst816s` - touch (or a small
  custom I2C driver copied from the reference).
- `espressif/esp_codec_dev` - ES8311 audio codec + I2S (speaker).
- `espressif/esp_jpeg` (or `espressif/esp_new_jpeg`) - JPEG decode to RGB565.
- `fatfs` / `sdmmc` are part of ESP-IDF.

## 4. Application architecture

```
moonstation/
  platformio.ini
  partitions_16mb.csv
  idf_component.yml          (managed deps)
  config.h.example           (tracked template)
  config.h                   (gitignored, local)
  main/
    CMakeLists.txt
    main.c                   (app_main, init sequence)
    pins.h                   (pin map, overridable via config.h)
    display.c/.h             (GC9A01 init, backlight, draw bitmap)
    sdcard.c/.h              (SDMMC mount, list /slides, read file)
    slideshow.c/.h           (scan, sort, decode, show, timer)
    touch.c/.h               (CST816D init, polling, events)
    audio.c/.h               (ES8311 + I2S init, test tone)
    jpeg_decode.c/.h         (decode JPEG to RGB565 buffer)
```

Init order in `app_main`:
1. NVS, default event loop.
2. Backlight on (active low).
3. Serial console.
4. Display init (SPI + GC9A01).
5. SD card mount.
6. Touch init (I2C + CST816D).
7. Audio init (ES8311 + I2S + PA).
8. Slideshow start.

## 5. Feature: slideshow

- On boot, mount SD; if missing, show `NO SD CARD` and retry every 2 s.
- Scan `/slides` for `*.jpg` / `*.jpeg`, sort by name.
- If none, show `NO SLIDES`.
- Loop forever: for each file, read into buffer, decode JPEG to a 240x240 RGB565
  frame, blit to display, wait `SLIDE_INTERVAL_MS`, then next. Wrap around.
- Touch tap advances to the next slide immediately (resets the timer).
- Long press or boot button could pause/resume (optional, phase 2).
- Decode errors: show `BAD IMAGE` and continue with the next file.
- Memory: allocate one frame buffer and one JPEG input buffer once; reuse them.

## 6. Feature: touch

- Init CST816D over I2C (port 1), reset sequence, probe 0x15.
- Poll every 10 ms (esp_timer) or use INT pin.
- Expose `touch_get_point()` and an event callback.
- If the chip is absent, log a warning and continue in non-touch mode
  (no crash).

## 7. Feature: speaker

- Init ES8311 over I2C (port 0) and I2S (MCLK/BCLK/WS/DIN/DOUT).
- Enable PA (GPIO 46).
- Play a short test tone on boot to prove the speaker works, then stay idle.
- Provide `audio_play_tone(freq, ms)` for feedback (e.g. on touch).
- Keep volume configurable via `config.h` (`AUDIO_VOLUME`, default 60).

## 8. config.h contract

`config.h.example` (tracked) ships defaults; `config.h` is gitignored and off
limits to agents. Macros (all with `#ifndef` fallbacks in code):

```
#define SLIDE_INTERVAL_MS 5000
#define SLIDE_DIR "/slides"
#define LCD_SPI_HZ 40000000
#define AUDIO_VOLUME 60
// pin overrides optional, defaults from pins.h
```

## 9. Milestones

1. **M0 - Skeleton**: PlatformIO project builds an empty `app_main` (hello world
   over USB serial). Flash and monitor work.
2. **M1 - Display**: GC9A01 init, backlight, fill screen, draw test pattern.
3. **M2 - SD card**: SDMMC mount, list root, read a file.
4. **M3 - JPEG slideshow**: decode and show one image, then loop over `/slides`.
5. **M4 - Touch**: CST816D init, polling, tap advances slide.
6. **M5 - Speaker**: ES8311 + I2S init, test tone on boot.
7. **M6 - Polish**: error messages on panel, config.h, README, robustness
   (card hot-plug, bad files), documentation.

## 10. Testing / verification

- Build: `~/.platformio/penv/bin/pio run` -> SUCCESS.
- Flash: `pio run -t upload --upload-port /dev/ttyACM0`.
- Serial: boot log shows display/SD/touch/audio init results.
- Visual: slideshow cycles images; touch advances; speaker beeps on boot.
- Edge cases: no SD card, empty `/slides`, corrupt JPEG, card inserted later.

## 11. Risks and notes

- No PSRAM: keep buffers small; one 240x240 RGB565 frame is ~115 KB.
- ESP-IDF 5.3 (PlatformIO) vs 6.0.2 (system): develop against PlatformIO's
  5.3.x; do not rely on 6.x APIs.
- GC9A01 vendor init sequence differs from the standard driver; reuse the
  sequence from the xiaozhi reference (invert color, mirror, custom params 0x62/0x63).
- I2C buses: touch on port 1, codec on port 0; do not mix them.
- Keep all identifiers, comments, serial output and UI strings in English.
- Never commit `config.h`; track only `config.h.example`.
