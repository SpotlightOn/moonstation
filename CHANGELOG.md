# Changelog

## Unreleased

- Decode slides with `espressif/esp_new_jpeg` (SIMD accelerated on the ESP32-S3)
  instead of the TJpgDec ROM codec. Measured on the board at 80 MHz over 30
  slides: decode time 189 ms -> 36 ms on average (5.3x), worst case 231 ms ->
  50 ms. Costs 72 kB flash and ~14 kB heap. The decoder handle stays open
  across slides and the panel frame buffer is 16-byte aligned for the SIMD
  path.
- Log read, decode and blit time per slide.

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
