# moonstation

ESP-IDF firmware for the Spotpear ESP32-S3 1.28" BOX board (GC9A01 240x240 round
display), built with PlatformIO. The initial feature is an endless slideshow of
240x240 JPEG images stored in `/slides` on the SD card. Touch and the built-in
speaker are functional.

## Layout

```
platformio.ini            build configuration
partitions_16mb.csv       16 MB flash layout, 4 MB application slot
src/                      firmware sources
scripts/build_slides.sh   convert a folder of images to 240x240 slides
config.h.example          tracked config template
config.h                  local settings, git-ignored
```

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
is git-ignored and must never be committed. Available macros:

| Macro | Default | Meaning |
| --- | --- | --- |
| `SLIDE_INTERVAL_MS` | `5000` | time per slide in milliseconds |
| `SLIDE_DIR` | `/slides` | directory on the SD card |
| `LCD_SPI_HZ` | `40000000` | panel SPI clock |
| `AUDIO_VOLUME` | `60` | speaker volume (0-100) |

## SD card

Format the card as FAT32. Create a `/slides` directory and copy 240x240 baseline
JPEG files into it. The firmware scans `/slides` on boot and loops through the
files in name order. Touching the screen advances to the next slide.

Use the helper script to prepare images:

```bash
./scripts/build_slides.sh /path/to/images
```

This writes 240x240 sRGB baseline JPEGs into `./slides`.

## Hardware

- ESP32-S3, 16 MB flash, no PSRAM, native USB on GPIO19/20.
- Panel: GC9A01, 240x240, inversion on, SPI 40 MHz.
- SD card: SDMMC 1-bit, CLK 17, CMD 18, D0 21, D3 13.
- Touch: CST816D on I2C (SDA 11, SCL 7).
- Audio: ES8311 on I2C (SDA 15, SCL 14), I2S MCLK 16, BCLK 9, WS 45,
  DIN 10, DOUT 8, PA enable 46.

## Notes

- Keep all identifiers, comments, serial output and UI strings in English.
- Never commit `config.h` or `.pio/`.
