# Radio mode: plan

Status: implemented (gesture layer, codec stream API, playlist parser, stream
tasks and the mode with its screen). What is left is polish, listed under
"Later"; `docs/development.md` has the maintainer summary.

## Goal

A fifth mode after the weather mode that plays an internet radio stream on the
built-in speaker. The stations are `.pls` playlist files on the SD card, and the
box is controlled by touch only, because the 240 px round panel has no room for
physical buttons.

Scope of the first version:

| In | Out |
| --- | --- |
| MP3 (MPEG-1/2 Layer III), 32-320 kbit/s, 44.1/48 kHz, mono and stereo | AAC, HE-AAC, Opus, Ogg Vorbis |
| `http://` streams | `https://` (TLS heap and CPU) |
| `.pls` playlists from `/radio` on the SD card | `.m3u`, `.xspf`, directories, nested folders |
| Gestures: tap, swipe left/right, swipe up/down, long press | Station list screen, ICY titles, recording |

MP3 first is a size decision: `chmorgan/esp-libhelix-mp3` is a pure C decoder of
about 30 kB, while AAC would pull in `espressif/esp_audio_codec` with a 20 MB
component archive. The decoder sits behind a small interface, so a later switch
does not touch the rest of the module.

## Data flow

```
HTTP GET station URL (Icy-MetaData: 0)
  -> read in 4 kB chunks into a 64 kB ring buffer in PSRAM  (~4 s at 128 kbit/s)
  -> MP3 sync + frame decode (helix)
  -> PCM 16 bit stereo at the stream rate
  -> esp_codec_dev_write()
  -> I2S -> ES8311 -> PA -> speaker
```

`Icy-MetaData: 0` keeps the title blocks out of the body, so the decoder never
sees text between frames. `esp_http_client` handles chunked encoding and
redirects; the body is read in a loop instead of `esp_http_client_perform()`,
which would block until the stream ends - and a radio stream never ends.

Two tasks, not one. The network task reads 4 kB blocks and may wait as long as
the server needs; the decoder task takes frames out of the ring and writes PCM.
They are separate because `esp_http_client_read()` returns only after the
**full** requested length, so a read inside the decode loop silences the I2S DMA
and is heard as a gap: with 48 kB reads and one task the spike measured 7.4 s of
audio per 12 s of wall clock with a gap every three seconds, after the split
12.0 s per 12 s with none.

The decoder task has an 8 kB stack (a stereo PCM frame alone is 4.6 kB) and
priority 5, the network task 4 kB and priority 4. The main task keeps running
the mode manager, so display, alarm and touch keep working while audio plays.

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

- Directory `settings.radio_dir`, default `/radio`, same idea as `slide_dir`:
  compiled in as `RADIO_DIR`, overridable on the setup page.
- Only files ending in `.pls` (case-insensitive), flat, sorted alphabetically,
  at most `RADIO_MAX_STATIONS` (32); the rest is ignored.
- One file is one station. `File1` plays, `File2`..`FileN` are mirrors tried
  when `File1` fails - the way Shoutcast directories hand out playlists.
- The name on screen is the file name without its extension: what the card's
  owner chose, and short. `Title1` is a subtitle, cut to fit; many stations put a
  whole description there (the SomaFM files carry "SomaFM: Groove Salad Classic
  (#1): The ...", useless as a name).
- Only absolute `http://` entries are used. A relative entry, an `https://` entry
  or a broken URL marks that mirror unusable; the screen says so instead of
  trying.
- `NumberOfEntries` is not trusted: the parser counts what it finds.
- The card is mounted on entering the mode and stays mounted (like the
  slideshow), so changing stations never waits for a remount. No file stays open
  between two reads.

Parsing in `src/radio/pls.c`: `fgets` line by line, trim `\r\n`, match keys
case-insensitively, keep up to four usable mirrors per station; file names come
from `readdir`, capped at 32 and sorted with `qsort`. Confirmed on the real card:
the seven files in `/radio` are one station each, and the SomaFM ones list four
mirrors of the same stream (`ice1`..`ice6.somafm.com/<name>-128-mp3`), so one
file stays one station and the mirrors only serve the retry chain.

## Gestures

The panel is the touch surface. The gestures are the same in every mode, so they
live above the mode - with one exception: the radio draws its own play/pause
button, because on this controller a tap is easy to produce by accident (the
first version had taps anywhere as play/pause and paused itself during a volume
slide).

| Gesture | Everywhere | In the radio mode |
| --- | --- | --- |
| Tap on the drawn button (< 500 ms, no movement) | mode-specific | play / pause |
| Tap above the button | volume +10 % | volume +10 % |
| Tap below the button | volume -10 % | volume -10 % |
| Swipe left / right | - | previous / next station |
| Swipe up / down | volume, one 5 % step per 40 px | same |
| Long press (>= 500 ms) | next mode | next mode |
| Hold 5 s without moving | setup page | setup page |

The split is deliberate: the destructive action (stopping the station) gets the
small, visible target, every other tap only changes the volume - a mistake there
costs one OSD bar, not the stream.

Implementation in `touch.c`:

- Track the largest distance from `start_x/start_y` over the whole touch, not just
  the position at release: the controller reports about every 100 ms, so a fast
  flick leaves one or two samples.
- On release, in this order: a setup hold wins, then a moved touch with
  `max(|dx|, |dy|) >= TOUCH_SWIPE_MIN_PX` (25) is a swipe along the dominant
  axis, then tap or long press. A swipe therefore never fires a tap or a mode
  switch, and it cancels the setup gesture like any other movement.
- Duration plays no part in that decision on purpose: a careful volume slide is
  slow and easily takes over a second; judged by time it became a long press and
  threw the radio out of its own mode. Distance alone separates a gesture from a
  press.
- `touch_set_swipe_cb(cb)` with `(dx, dy, duration_ms)` fires once per gesture;
  the volume turns the travel into steps (one per 40 px, at most five).
- The tap callback carries the landing point (`tap_cb(x, y)`), so a mode can turn
  parts of the screen into buttons without guessing where the finger was.

Threading: callbacks run in the touch task and the display must not be pushed
from two tasks, so the swipe callback only records a pending step (a signed
integer) and wakes the main task, which applies the volume and draws the bar.

## Volume as a firmware function

`settings.audio_volume` exists and is on the setup page; the gesture makes it a
runtime control:

- `slideshow_volume_steps()` in `slideshow.c`: clamp to 0..100, apply, remember
  that a save is due.
- `audio_set_volume(int percent)` in `audio.c` sets the codec volume directly
  when a stream is open, so the change is audible while music plays. Tones keep
  taking their volume from `settings.audio_volume`.
- Saved once per gesture via `settings_save()` when the finger is lifted, not per
  step; NVS is flash and does not like a write every 100 ms.
- `ALARM_MIN_VOLUME_PCT` (70) still wins while the alarm rings: turning the
  feedback volume down cannot silence it.

## On-screen display

`src/ui/osd.c` draws over whatever the active mode put on the panel, without the
mode knowing about it:

- Copy the covered rectangle (152x64 px in the middle, about 15 kB) into PSRAM,
  draw the bar into the shared frame buffer, push only that rectangle with
  `esp_lcd_panel_draw_bitmap()`.
- After 1.5 s the copy is restored. A checksum decides whether that is still
  safe: if a mode pushed a full frame in the meantime (through the shared
  `ui_push()` helper), the restore is dropped instead of painting old pixels over
  new ones.
- The deadline feeds the main loop's wake-up (`osd_next_wake_us()`), so no timer
  of its own is needed.

That keeps the OSD usable in the clock, the slideshow (no JPEG decode needed to
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
  `net_wifi_hold()`. No WiFi or no playlist: own hint screen, no task, no error
  churn.
- `radio_exit()`: stop the task, close the codec, release power and WiFi, free
  the ring. Nothing plays outside the mode.
- Tap on the button toggles pause (the task stops reading and the codec closes;
  resuming reconnects). Any other tap changes the volume.
- Station changes and pause come from the main task, which owns the mode; the
  touch callback only stores the request and the point it landed on.
- Swipe left/right: previous/next station, wraps around, starts immediately.
- Failure: retry the same URL after 2 s, 5 s, 10 s and then the next mirror;
  while it fails the screen says `No signal` and a tap on the button retries at
  once. A station that plays again resets the backoff.
- Underrun: the decoder starts once ~24 kB (~1.2 s) are in the ring and waits for
  more when it runs empty, showing "Buffering". The ring is 64 kB, about three
  seconds at 160 kbit/s.
- Sample rate and bitrate come from the first decoded frame and are shown; the
  codec is opened with that rate once per station.

Screen (screenshot name `radio`), top to bottom, centred:

| y | Content |
| --- | --- |
| 8 | `VOL +`, dim - the louder half of the screen |
| 54 | station name (file name), cut to fit |
| 78 | `Title1`, cut, dim - omitted when empty |
| 96..144 | button: ring with pause bars (`playing`) or a play triangle |
| 152 | `Playing`, `Paused`, `Buffering`, `No signal` |
| 174 | `MP3 160 kbit/s 44.1 kHz` |
| 196 | `2 / 7` station position, dim |
| 214 | `VOL -`, dim - the quieter half |

## Audio path and codec ownership

The codec used to be opened and closed around every tone; the radio keeps it
open, so the two have to agree (`audio.c`):

- `audio_stream_begin(int sample_rate)`, `audio_stream_write(const void *pcm,
  size_t bytes)`, `audio_stream_end()`. `begin` opens the codec with
  `esp_codec_dev_sample_info_t` at the stream's rate, two channels, 16 bit.
- A mutex guards open/close/write, because the decode task and the main task
  (alarm beeps, feedback tones) both use the codec.
- A tone that arrives while a stream is open is written into the same codec at
  the stream's rate: the decoder waits on the lock and resumes afterwards, so an
  alarm never clicks and never changes the rate under the decoder.
- Mono streams are duplicated to stereo in the decode loop; the ES8311 path stays
  at two channels either way.

## Power, heat and runtime

- Playing: `POWER_PROFILE_BOOST` (240 MHz) and WiFi without power save - MP3
  decode at 80 MHz in parallel with a 40 MHz display SPI is too tight.
- Estimate 120-200 mA while playing: a few hours on battery instead of days. The
  mode is meant for the charger or a session, not all night.
- Deep sleep only happens in the clock mode, so nothing has to be disabled;
  leaving the mode releases both holds and the box returns to its idle behaviour.
- Backlight dimming stays as it is; a swipe or tap wakes the screen as usual.

## Settings, config and web form

- `RADIO_ENABLE` in `features.h` (default 1, `0` removes the mode from the cycle
  and the `radio_init()` call). The module is compiled either way, so CI keeps
  checking it; a box without playlists shows a hint, so the switch costs nothing.
- `RADIO_DIR "/radio"` in `config.h.example` plus an `#ifndef` fallback in
  `settings.c`.
- `settings_t` grows `char radio_dir[32]` (appended last, so the blob migration
  keeps working), `SETTINGS_VERSION` 6 -> 7, `sanitize()` falls back to
  `RADIO_DIR` when the value is empty.
- Setup page: one more field, `FIELDS` 29 -> 30.
- `radio_init()` at boot (like `weather_init()`) only clears state; nothing
  touches the network before the mode is entered.

## Risks

1. **Component vs IDF 6.1**: `esp-libhelix-mp3` 1.0.3 is tested against IDF 5.x.
   It is plain C with no IDF API use, and the spike proved it compiles and
   decodes 44.1 kHz stereo on IDF 6.1. `chmorgan/esp-audio-player` 2.0.0 would
   save the HTTP loop but adds a dependency that is also only tested on 5.x.
2. **Fast swipes**: the controller reports at roughly 10 Hz. The classifier uses
   the farthest point of the touch, not the last sample, or short flicks would
   look like taps.
3. **Codec clicks**: opening and closing the ES8311 with the PA enabled pops. The
   stream is opened once per station, and the PA is toggled around the open, not
   per buffer.
4. **Buffer size**: 64 kB is four seconds at 128 kbit/s but two at 256 kbit/s. A
   station that needs more shows "Buffering" more often; the size is a macro.
5. **No HTTPS**: many directory streams are HTTP-only, which is why the first
   version stops there. TLS later costs ~40 kB heap per connection plus CPU, and
   needs `POWER_PROFILE_BOOST` even for low bitrates.
6. **Heat**: charging plus streaming plus display is the warmest state the box
   reaches - worth watching on hardware, not worth designing around yet.

## Later

- ICY titles (send `Icy-MetaData: 1`, split the `StreamTitle` blocks out of the
  body) as a second screen line.
- `.m3u` next to `.pls`; both are one line of extension matching apart.
- HTTPS, then AAC via `esp_audio_codec`.
- Keep playing while another mode shows, with the radio as a background source
  and a small indicator in the status row.
- A station list screen (tap to open, swipe to scroll) if the mirror assumption
  turns out wrong or the number of playlists grows.
