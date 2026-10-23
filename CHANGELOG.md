# Changelog

## Unreleased

- Deep sleep for the clock mode inside a configurable night window.
- Local time instead of UTC for the clock and the sleep window.
- Fix clock time: summer time in the compile time, wait for the real NTP answer.
- Daily NTP resync at `NTP_DAILY_SYNC_HOUR`, `NTP_SERVER` honoured.

## 0.2.0 - 2026-10-22

- Add battery indicator.
- Add GitHub Actions CI (cppcheck, firmware build) and tagged releases.
- Build with ESP-IDF 6.1 (PlatformIO platform 8.0.0).
- Initial version.
