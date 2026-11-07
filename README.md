# Moonstation

[![CI](https://github.com/SpotlightOn/moonstation/actions/workflows/ci.yml/badge.svg)](https://github.com/SpotlightOn/moonstation/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/SpotlightOn/moonstation?label=release&sort=semver)](https://github.com/SpotlightOn/moonstation/releases/latest)
[![Platform](https://img.shields.io/badge/platform-ESP32--S3%20%7C%20ESP--IDF%206.1-informational)](docs/hardware.md)

ESP-IDF firmware for the Spotpear ESP32-S3 1.28" BOX board (GC9A01 240x240
round display), built with PlatformIO. It shows an endless slideshow of JPEGs
from an SD card, a clock with date and battery, a weather mode, an MP3 internet
radio and an H.264 movie player, switched by a long press on the touch screen.
A 5 s hold opens an HTTPS setup page for everything the owner tunes; a box that
has never been on a network opens its own access point for that page, so the
first setup needs no cable.

![Spotpear Box 1.28](docs/spotpear-box.avif)
https://www.aliexpress.com/item/1005010288236253.html

## Features

- Six modes on a long press: clock → slideshow → clock+slideshow → weather →
  radio → movie; the box boots into the clock
- Slideshow of 240x240 baseline JPEGs in name order, optional crossfade; a tap
  advances immediately
- Clock with HH:MM, weekday, date and battery, redrawn on the minute; an
  optional night deep sleep that a touch or a pending alarm interrupts
- Weather (`WEATHER_ENABLE`): current conditions, 24 hours and five days on
  successive taps, cached in NVS and refreshed every 20 minutes
- Radio (`RADIO_ENABLE`): MP3 streams from `.pls` playlists, one file per
  station; the drawn button plays and pauses, swipes change station and volume,
  and errors are shown and retried
- Movies (`MOVIE_ENABLE`): H.264 with MP3 sound from `/movies`, decoded in
  software; tap to play and pause, swipe for the next or previous, a movie that
  ends starts over
- HTTPS setup page with login: folders, timings, backlight, night window,
  timezone, alarm, audio, battery and weather, stored in NVS and applied at
  runtime; a red dot shows it runs
- First setup without a cable: a box without WiFi opens `Moonstation-XXXX` at
  192.168.4.1 and serves the page there
- No secrets in the source: WiFi, the login and the weather key are entered on
  the page, stored in NVS and never shown again
- Dual boot with xiaozhi-esp32: hold the BOOT button for two seconds, works in
  both directions
- Volume swipe in every mode, a daily alarm with its own screen, backlight
  dimming, touch feedback, and a red or yellow screen for a missing SD card or
  an empty image directory

## Prerequisites

- PlatformIO (use the bundled venv binary shown below)
- Python 3 with Pillow (only for regenerating the fonts)
- ImageMagick 7 (`magick`) for preparing slide images
- FFmpeg with libx264 and libmp3lame for preparing movies
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

The upload writes the bootloader, the partition table and the app - and with it
the layout the second firmware (xiaozhi-esp32) shares. The one-time step for
boxes that still carry the old single-app table and all the details live in
[docs/dual-boot.md](docs/dual-boot.md).

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

The access point is open on purpose: it serves nothing but the login form,
which has to be replaced before anything else appears, and it disappears once
WiFi is configured. If the box later cannot reach its network (moved house, new
router), the 5 s gesture falls back to the same access point.

## SD card

Format the card as FAT32 and place 240x240 baseline JPEGs in the configured
directory; the firmware scans it on boot and loops through the files in name
order. `scripts/build_slides.sh` converts arbitrary images with ImageMagick 7:

```bash
./scripts/build_slides.sh /path/to/images
./scripts/build_slides.sh /path/to/images /path/to/output
```

Then copy the output to the card.

## Documentation

- [docs/movie.md](docs/movie.md): converting and playing movies, the container,
  decoder notes
- [docs/radio.md](docs/radio.md): the radio mode, playlists, stream tasks,
  codecs
- [docs/dual-boot.md](docs/dual-boot.md): the shared flash with xiaozhi-esp32,
  the switch, update rules
- [docs/configuration.md](docs/configuration.md): every `config.h` macro, the
  credentials model, NTP and deep sleep
- [docs/setup-page.md](docs/setup-page.md): the HTTPS setup page and its
  behaviour
- [docs/hardware.md](docs/hardware.md): the board, its pins and peripherals
- [docs/development.md](docs/development.md): internals for maintainers

CI runs cppcheck and a full firmware build on every push to `main` and on pull
requests; pushing a `v*` tag publishes a GitHub Release with the binaries.
