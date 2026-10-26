# Changelog

## Unreleased

**New**

- Daily alarm, set on the setup page (`ALARM_ENABLED`, `ALARM_HOUR`,
  `ALARM_MINUTE`): own ringing screen, two rising tones, stops on a tap or a
  long press, gives up after five minutes, a changed time applies at once.
- The setup page, its stylesheet and scripts are ordinary files under
  `src/web/`, inlined into `pages.h` before every build.

## 0.4.1 - 2026-10-25

**Changed**

- Idle drops to 10 MHz (`PM_MIN_CPU_MHZ`) instead of 40 MHz: the power manager
  is configured once at boot, and only a running task or a driver lock keeps the
  clock up.
- Clock ceilings live in one place: `src/core/power.c` with the refcounted
  profiles `ACTIVE` (80 MHz) and `BOOST` (240 MHz while the setup page runs).
  Modules request a profile instead of reconfiguring the power manager
  themselves.

## 0.4.0 - 2026-10-25

**New**

- First setup without a cable: a box without WiFi credentials opens its own
  access point (`Moonstation-XXXX`, `192.168.4.1`) at boot and serves the setup
  page there; the 5 s gesture falls back to the same access point when the home
  network cannot be reached.
- Weather mode (`WEATHER_ENABLE`, on by default): current conditions plus a
  24-hour forecast that a tap flips through, cached in NVS and refreshed every
  `WEATHER_REFRESH_MIN` minutes by coordinate. Without a key the mode shows a
  hint instead of a screen.
- Proportional body font for dates, labels and readings; the clock keeps its
  bold 48 px font. Weekday in front of the date (`Sun, 25.10.2026`).
- Optional frame dump as a BMP on the SD card for layout debugging
  (`SCREENSHOT_ENABLE`, off by default).

**Changed**

- The setup page starts with the factory login `admin` / `admin` and serves
  only the form that replaces it until a new one is saved; the login is never
  empty, and "Reset to default" keeps it. Passwords and the weather key are
  write-only, and no credential is part of the config template any more.
- The setup page raises the CPU to 240 MHz while it is open and hands the clock
  back to the power manager when it closes.
- Optional crossfade between two images (`SLIDE_FADE_MS`, default 0), decoded
  into a PSRAM frame and mixed into the panel band by band.
- Slides are decoded with `espressif/esp_new_jpeg` instead of the TJpgDec ROM
  codec: 189 ms -> 36 ms on average over 30 slides (5.3x) at 80 MHz.
- Optional automatic light sleep (`PM_LIGHT_SLEEP`, off by default) and a CPU
  speed lock that is only held around the image work.
- Group the setup page into labeled fieldsets.

**Fixed**

- The deepest backlight step blanked the screen after 90 s of idle outside the
  night window, and the wake-up at the end of the night left it dark until the
  next touch. It now applies inside the window only.

## 0.3.0 - 2026-10-23

- Setup web UI over HTTPS on a 5 s hold, with login and reset to default.
- Settings page on static HTML: native validation, checkboxes, default hints.
- Slide directory and interval (seconds) editable at runtime.
- Night deep sleep switchable at runtime.
- Runtime settings in NVS, `config.h` only seeds the defaults.
- Compile-time feature switches in `src/config/features.h`.
- Local time instead of UTC.
- Fix clock time: summer time, wait for the NTP answer.
- Daily NTP resync at `NTP_DAILY_SYNC_HOUR`.
- Serial capture recipe in docs/development.md.

## 0.2.0 - 2026-10-22

- Add battery indicator.
- Add GitHub Actions CI (cppcheck, firmware build) and tagged releases.
- Build with ESP-IDF 6.1 (PlatformIO platform 8.0.0).
- Initial version.
