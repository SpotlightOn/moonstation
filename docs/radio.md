# Radio mode: plan

Status: planned, not implemented. This is the design agreed before the first
line of code; `docs/development.md` gets the section once the mode exists.

## Goal

A fifth mode after the weather mode that plays an internet radio stream on the
built-in speaker. The stations are `.pls` playlist files on the SD card, and
the box is controlled only by touch gestures, because the 240 px round panel
has no room for buttons.

Scope of the first version:

| In | Out |
| --- | --- |
| MP3 (MPEG-1/2 Layer III), 32-320 kbit/s, 44.1/48 kHz, mono and stereo | AAC, HE-AAC, Opus, Ogg Vorbis |
| `http://` streams | `https://` (TLS heap and CPU) |
| `.pls` playlists from `/radio` on the SD card | `.m3u`, `.xspf`, directories, nested folders |
| Gestures: tap, swipe left/right, swipe up/down, long press | Drawn buttons, ICY titles, recording |

MP3 first is a size decision, not a taste one: `chmorgan/esp-libhelix-mp3`
(the reference decoder, in most ESP32 players) is a pure C decoder of about
30 kB. AAC would mean `espressif/esp_audio_codec`, whose component archive is
20 MB and only worth pulling in when AAC is really wanted. The decoder sits
behind a small interface, so a later switch does not touch the rest of the
module.

## What is already there

| Piece | State |
| --- | --- |
| I2S TX + ES8311 + PA | `src/core/audio.c` plays tones at 24 kHz, volume per call |
| WiFi without power save | `net_wifi_hold()` sets `WIFI_PS_NONE`, `net_wifi_release()` ends it |
| Mode manager | `ui_mode_t` in `slideshow.c`, a new mode is one struct plus a `next` link |
| Touch | `touch.c` already reads X/Y and tracks `start_x/start_y`, fires tap and long press on release |
| Settings + setup page | `settings_t`, the `FIELDS` table in `webserver.c` still has room (29 of 32 entries) |
| SD card | `sdcard_init()` / `sdcard_is_mounted()`, the card stays mounted while a mode needs it |
| Power profiles | `power_request(POWER_PROFILE_BOOST)` for CPU-heavy work |

Missing: a stream API on the codec, a swipe callback, an on-screen volume bar,
and the radio module itself.

## Data flow

```
HTTP GET station URL (Icy-MetaData: 0)
  -> read in 4 kB chunks into a 64 kB ring buffer in PSRAM  (~4 s at 128 kbit/s)
  -> MP3 sync + frame decode (helix)
  -> PCM 16 bit stereo at the stream rate
  -> esp_codec_dev_write()
  -> I2S -> ES8311 -> PA -> speaker
```

`Icy-MetaData: 0` keeps the title blocks out of the body, so the stream is a
plain MP3 elementary stream and the decoder never sees text between frames.
`esp_http_client` handles chunked transfer encoding and redirects; the body is
read in a loop instead of `esp_http_client_perform()`, which would block until
the stream ends - and a radio stream never ends.

The stream runs in its own task (8 kB stack, priority 5, fixed to the same
core as the mode manager is not required). The main task keeps running the
mode manager, so the display, the alarm and the touch keep working while audio
plays.

## Playlists

A `.pls` file is INI-shaped:

```
[playlist]
NumberOfEntries=2
File1=http://stream.example.org:8000/live
Title1=Example Radio
File2=http://backup.example.org:8000/live
Length1=-1
Version=2
```

Rules of the first version:

- Directory `settings.radio_dir`, default `/radio`, same idea as `slide_dir`.
  Compiled in as `RADIO_DIR`, overridable at runtime on the setup page.
- Only files ending in `.pls` (case-insensitive), flat, sorted alphabetically.
  At most `RADIO_MAX_STATIONS` (32) entries; the rest is ignored.
- One file is one station. `File1` is the address that plays, `File2`..`FileN`
  are mirrors that are tried when `File1` fails. This matches how Shoutcast
  directories hand out playlists today.
- The name on screen is `Title1`, falling back to the file name without its
  extension, cut to what fits the panel.
- Only absolute `http://` entries are used. A relative entry, an `https://`
  entry or a broken URL marks that mirror as unusable; the screen then says so
  instead of trying.
- `NumberOfEntries` is not trusted: the parser counts what it actually finds.
- The card is mounted on entering the mode and stays mounted (the slideshow
  does the same), so switching stations never waits for a remount. No file
  stays open between two reads.

Parsing ~120 lines in `src/radio/pls.c`: read line by line with `fgets`, trim
`\r\n`, match the key case-insensitively, keep the first 32 usable entries.
File names come from `readdir`, capped and sorted with `qsort`.

**To confirm before the code is written:** a `.pls` from a radio directory
lists mirrors of one station, which is what this plan assumes. If the files
should instead list several different stations, the gesture set changes (a tap
inside the mode would switch station, not pause) and the parser keeps every
`FileN` as a station of its own. Everything else stays the same.

## Gestures

The panel is the touch surface; there are no fixed buttons, and drawing three
small ones on 240 px is worse than a swipe. The gestures are the same in every
mode, so they live above the mode, not inside the radio:

| Gesture | Everywhere | In the radio mode |
| --- | --- | --- |
| Tap (< 500 ms, no movement) | mode-specific | play / pause |
| Swipe left / right | - | previous / next playlist |
| Swipe up / down | volume +/- 5 % with a bar | volume +/- 5 % with a bar |
| Long press (>= 500 ms) | next mode | next mode |
| Hold 5 s without moving | setup page | setup page |

Implementation in `touch.c`:

- Track the largest distance from `start_x/start_y` over the whole touch, not
  just the position at release: the controller reports about every 100 ms, so a
  fast flick leaves only one or two samples.
- On release, in this order: a setup hold wins, then a moved touch with
  `max(|dx|, |dy|) >= TOUCH_SWIPE_MIN_PX` (50) and a duration below
  `TOUCH_SWIPE_MAX_MS` (1000) is a swipe along the dominant axis, then the
  existing tap or long press. A swipe therefore never fires a tap.
- A swipe already cancels the setup gesture (`TOUCH_SETUP_MOVE_PX` is 40), so
  the two cannot collide.
- New callback `touch_set_swipe_cb(cb)` with `(dx, dy, duration_ms)`, fired once
  per gesture. One swipe is one step; holding a finger and dragging further
  does not repeat.

Threading: callbacks run in the touch task, and the display must not be pushed
from two tasks. The swipe callback therefore only records a pending step (a
signed integer) and wakes the main task, exactly like `weather_on_tap()` sets
a flag today. The main task applies the volume and draws the bar.

## Volume as a firmware function

`settings.audio_volume` already exists and is already on the setup page. The
gesture makes it a runtime control:

- `ui_volume_step(int delta)` in `slideshow.c`: clamp to 0..100, apply, remember
  that a save is due.
- New `audio_set_volume(int percent)` in `audio.c`: sets the codec volume
  directly when a stream is open, so the change is audible while music plays.
  Tones keep taking their volume from `settings.audio_volume` as they do now.
- Saved once per gesture via `settings_save()` when the finger is lifted, not
  per step; NVS is flash and does not like a write every 100 ms.
- `ALARM_MIN_VOLUME_PCT` (70) still wins while the alarm rings: turning the
  feedback volume down cannot silence the alarm.

## On-screen display

`src/ui/osd.c` draws over whatever the active mode put on the panel, without the
mode knowing about it:

- Copy the rectangle the bar will cover (168x44 px at y = 98, about 15 kB) into
  a PSRAM buffer.
- Draw the bar (volume) or a play/pause glyph into the frame buffer, push just
  that rectangle with `esp_lcd_panel_draw_bitmap()`.
- After 1.5 s the main loop restores the saved rectangle. A mode that pushed a
  fresh full frame in the meantime marks the copy stale - the restore is then
  dropped instead of painting old pixels over new ones. The shared full-frame
  push gets one helper (`ui_push(fb)`) for that, instead of every mode having
  its own.
- The timer rides on the main loop's wake-up: `osd_next_wake_us()` in the same
  minimum calculation as the modes and the alarm.

This keeps the OSD usable in the clock, the slideshow (no JPEG decode needed to
bring the screen back) and the radio.

## Radio module

```
src/radio/radio.c/.h   mode: state, screen, enter/exit, tap and swipe handling
src/radio/pls.c/.h     playlist scanning and parsing
src/radio/stream.c/.h  HTTP read loop, ring buffer, decode, reconnect
```

State machine: `Idle -> Connecting -> Buffering -> Playing -> (Paused |
Reconnecting | Failed)`.

- `radio_enter()`: ensure the card is mounted, scan and parse the playlists,
  remember the index, start the stream task, take `POWER_PROFILE_BOOST` and
  `net_wifi_hold()`. No WiFi configured or no playlist: own hint screen, no
  task, no error churn.
- `radio_exit()`: stop the task, close the codec, release power and WiFi, free
  the ring buffer. Nothing plays outside the mode.
- Tap: toggle pause (task stops reading and the codec is closed after a short
  fade-less stop; the buffer keeps its content).
- Swipe left/right: previous/next playlist, wraps around, starts immediately.
- Failure: retry the same URL after 2 s, then 5 s, then 10 s, then the next
  mirror; after all mirrors the screen says "Station unreachable" and a tap
  retries. A stream that plays again resets the backoff.
- Underrun: playback waits until ~8 kB are buffered (~0.5 s) and resumes; the
  screen shows "Buffering" meanwhile.
- Sample rate and bitrate come from the first decoded frame and shown on
  screen; the codec is opened with that rate once per station.

Screen (screenshot name `radio`), top to bottom, centred:

| y | Content |
| --- | --- |
| 70 | station title, cut to fit |
| 100 | `Playing`, `Paused`, `Buffering`, `No signal` |
| 130 | `MP3 128 kbit/s 44.1 kHz` |
| 162 | `2 / 5` playlist position, dim |

## Audio path and codec ownership

The codec is currently opened and closed around every tone. The radio keeps it
open, so the two have to agree:

- `audio_stream_begin(int sample_rate)`, `audio_stream_write(const void *pcm,
  size_t bytes)`, `audio_stream_end()` in `audio.c`. `begin` opens the codec
  with `esp_codec_dev_sample_info_t` at the stream's rate, two channels, 16 bit.
- A mutex in `audio.c` guards open/close/write, because the decode task and the
  main task (alarm beeps, feedback tones) both use the codec.
- Before a tone plays, `audio_play_tone_at()` asks the radio to pause: a flag
  plus a semaphore, so the decode task stops writing and releases the lock, the
  tone plays, then the stream resumes. Without that, an alarm in the middle of
  a song would either be silent or click.
- Mono streams are duplicated to stereo in the decode loop; the ES8311 path
  stays at two channels either way.

## Power, heat and runtime

- Playing: `POWER_PROFILE_BOOST` (240 MHz) and WiFi without power save. MP3
  decode at 80 MHz in parallel with a 40 MHz display SPI is too tight.
- Estimate 120-200 mA while playing: a few hours on battery instead of days.
  The mode is meant for the charger or a session, not all night.
- Deep sleep only happens in the clock mode, so nothing has to be disabled for
  the radio. Leaving the mode releases both holds and the box returns to its
  usual idle behaviour.
- Backlight dimming stays as it is; a swipe or tap wakes the screen as usual.

## Settings, config and web form

- `RADIO_ENABLE` in `features.h` (default 1, `0` removes the mode from the cycle
  and the `radio_init()` call). The module is compiled either way, so CI keeps
  checking it. A box without playlists shows a hint, so the switch costs
  nothing.
- `RADIO_DIR "/radio"` in `config.h.example` plus an `#ifndef` fallback in
  `settings.c`.
- `settings_t` grows `char radio_dir[32]` (appended last, so the blob migration
  keeps working), `SETTINGS_VERSION` 6 -> 7, `sanitize()` falls back to
  `RADIO_DIR` when the value is empty.
- Setup page: one more field, `FIELDS` 29 -> 30.
- `radio_init()` at boot (like `weather_init()`) only clears state; nothing
  touches the network before the mode is entered.

## Effort

| Step | Lines | Order |
| --- | --- | --- |
| Swipe callback, shared volume step, OSD | ~280 | 1 |
| Codec stream API and mutex | ~90 | 2 |
| Spike: one hardcoded stream plays | ~120 | 3 |
| Playlist scan, parser, mode and screen | ~400 | 4 |
| Reconnect, underrun, docs, changelog | ~120 | 5 |

Steps 1 and 2 are useful without the radio (volume gesture in every mode),
which is why they come first and get their own commit.

## Risks

1. **Component vs IDF 6.1**: `esp-libhelix-mp3` 1.0.3 is tested against IDF 5.x.
   It is plain C with no IDF API use, so the risk is low, but the spike has to
   prove it compiles and decodes. `chmorgan/esp-audio-player` 2.0.0 would save
   the HTTP loop, but adds a dependency that is also only tested on 5.x; the
   direct helix loop stays the fallback.
2. **Fast swipes**: the controller reports at roughly 10 Hz. The classifier uses
   the farthest point of the touch, not the last sample, or short flicks would
   look like taps.
3. **Codec clicks**: opening and closing the ES8311 with the PA enabled pops.
   The stream is opened once per station, and the PA enable is toggled around
   the open, not per buffer.
4. **Buffer size**: 64 kB is four seconds at 128 kbit/s but two at 256 kbit/s.
   A station that needs more shows "Buffering" more often; the size is a macro.
5. **No HTTPS**: many directory streams are HTTP-only, which is why the first
   version stops there. Adding TLS later costs ~40 kB heap per connection and
   CPU time, and needs `POWER_PROFILE_BOOST` even for low bitrates.
6. **Heat**: charging plus streaming plus display is the warmest state the box
   reaches. Worth watching on real hardware, not worth designing around yet.

## Later

- ICY titles (send `Icy-MetaData: 1`, split the `StreamTitle` blocks out of the
  body) as a second screen line.
- `.m3u` next to `.pls`; both are one line of extension matching apart.
- HTTPS, then AAC via `esp_audio_codec`.
- Keep playing while another mode shows, with the radio as a background source
  and a small indicator in the status row.
- A station list screen (tap to open, swipe to scroll) if the mirror assumption
  turns out wrong or the number of playlists grows.
