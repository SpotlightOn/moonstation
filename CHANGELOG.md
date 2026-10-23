# Changelog

## Unreleased

- Setup web UI over HTTPS, opened by holding the finger still for 5 s:
  settings form, optional login, "Reset to default" that keeps the WiFi
  credentials, red dot while it runs.
- Serial capture recipe with a DTR/RTS reset in docs/development.md.
- Compile-time feature switches in `src/config/features.h`.
- Deep sleep for the clock mode inside a configurable night window.
- Local time instead of UTC for the clock and the sleep window.
- Fix clock time: summer time in the compile time, wait for the real NTP answer.
- Daily NTP resync at `NTP_DAILY_SYNC_HOUR`, `NTP_SERVER` honoured.
- Move configuration to runtime settings (`src/config/settings.c`): `config.h`
  seeds the defaults, NVS stores the rest.

## 0.2.0 - 2026-10-22

- Add battery indicator.
- Add GitHub Actions CI (cppcheck, firmware build) and tagged releases.
- Build with ESP-IDF 6.1 (PlatformIO platform 8.0.0).
- Initial version.
