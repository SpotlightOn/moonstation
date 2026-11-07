# Movie mode: plan

Status: implemented (container format, playback engine, mode with its screen
and gestures, converter script). Open points are listed under "Later";
`docs/development.md` has the maintainer summary.

## Goal

A sixth mode after the radio that plays short videos on the round panel, with
their sound, from the SD card. The box has no buttons, so everything is a
touch gesture, and the ESP32-S3 has no video hardware: an H.264 software
decoder has to keep up with a 240x240 panel on a chip that idles at 80 MHz.

## Scope of the first version

| In | Out |
| --- | --- |
| H.264 constrained baseline, Annex-B, 240x240, 15 fps | other codecs (VP8/VP9, HEVC), higher resolutions, hardware decode (P4 only) |
| MP3 audio at 44.1 kHz mono | AAC/AC-3 passthrough, more than one audio track, subtitles |
| MPEG-TS (`.ts`) files from `/movies`, played in name order | MKV/MP4 demuxing on the device |
| Gestures: tap, swipe left/right, swipe up/down, long press | seeking inside a movie, a playlist screen |
| Loop the current movie | auto-advance through the folder |

## Why MPEG-TS

The decoder does not tell the firmware the frame rate, and a raw elementary
stream has nowhere to keep one; the audio track needs a place in the same file
anyway. A container of our own would mean nobody could look at a movie without
converting it first.

MPEG-TS is the cheapest standard container for this firmware:

| Property | Why it matters here |
| --- | --- |
| H.264 stays Annex-B | no AVCC conversion, no `CodecPrivate`; MP4 and MKV both store length-prefixed NAL units, TS does not |
| One stream per PID instead of a sample table | no index to parse, no offsets to seek; the demuxer reads 188 byte packets and copies the payload |
| MP3 is a standard stream type (0x03) | the radio's MP3 decoder works unchanged, AAC would not |
| VLC, mpv and ffmpeg open the file as it is | a movie on the card can be checked on a desktop without a conversion step |
| Timestamps in the PES headers | the frame rate comes from the stream itself, not from a header we would have to maintain |

The cost is about 15 % of file size: every PES ends in stuffing because a
transport packet is a fixed 188 bytes and a frame is rarely a multiple of it.
A two-minute movie is roughly 4.5 MB instead of 3.9 MB.

`src/movie/ts.c` is the whole demuxer: PAT/PMT once, then per packet the PID,
the adaptation field and the PES header, and the payload goes out. No section
parsing beyond the two tables, no seeking, no PCR handling.

## Data flow

```
scripts/build_movies.py (on the PC)
  ffmpeg: scale to fill and center-crop 240x240, 15 fps, x264 constrained
          baseline, Annex-B
  ffmpeg: mp3lame 48 kbit/s, 44.1 kHz, mono, padded to the video length
  ffmpeg: both raw streams copied into MPEG-TS packets, without re-encoding
  -> one .ts file

on the device
  open the file, PAT/PMT -> PID of the picture and of the sound, frame rate
  from the first PES timestamps
  video task: 256 kB window of the Annex-B stream -> esp_h264 software decoder
              (I420) -> YUV to RGB565, centered in the shared frame buffer
              -> SPI push -> wait for the next frame slot (esp_timer)
  audio task: MP3 decode (helix) -> esp_codec_dev_write -> I2S DMA
```

Two tasks because the two clocks are independent: the I2S DMA paces the sound
by itself, and the picture has to wait for its frame slot even when the decode
took longer or shorter than 1/15 s. A single task would have to interleave both
and would starve the codec during a 23 ms SPI push.

## Preview on the desktop

A `.ts` movie plays in VLC, mpv and ffmpeg without any conversion.
`scripts/preview_movie.py` prints what is inside, checks it against what the box
can do (H.264, at most 240x240, at most 20 fps) and then plays it with ffplay;
`-o` writes a Matroska copy.

The decoder takes the byte stream, not single NAL units: `esp_h264_dec_process()`
finds the access units itself and reports how many bytes it consumed. Feeding
it one NAL unit per call ends in `ACCESS UNIT BOUNDARY CHECK` /
`failed to activate param sets`, which is how the first version of this module
failed on every file. The task therefore keeps a 256 kB window of the video
stream, hands it over, drops the consumed front, refills from the card and only
stops when the stream is empty.

The decoded picture goes into the frame buffer the other modes share instead of
a buffer of its own: the panel's SPI DMA cannot read PSRAM, and the bounce
buffer the LCD driver would allocate for one full frame (115 kB) does not fit
into internal RAM next to the decoder. What is on the panel is therefore always
the shared buffer, which the pause overlay dims in place.

A file that is not a transport stream is refused after the first read instead
of being searched for a program table.

## Memory and performance

| Item | Where | Size |
| --- | --- | --- |
| Decoder state and reference frames | PSRAM (component allocation) | ~0.5-1 MB |
| Video window | PSRAM | 256 kB |
| Picture buffer | internal, shared with the other modes | 115 kB (static, already there) |
| MP3 input buffer | PSRAM | 16 kB |
| PCM frame | internal | 4.6 kB |
| TS reader per stream (packet, pending payload) | internal | ~0.4 kB |

Espressif's benchmark of the decoder is 23-30 fps at 320x192, but that is the
decoder alone. On the box, 240x240 at 15 fps does not keep up once a sound
track runs next to it: the picture task falls behind its frame slot, decodes
without pause to catch up, and the result stutters and drifts away from the
sound. 12 fps with sound, or 192x192 at 15 fps, is what stays smooth; that is
what the converter's warning is about. `POWER_PROFILE_BOOST` (240 MHz) is held
for the whole playback, the same choice the radio makes; 80 MHz is not enough
for decode plus a 40 MHz SPI stream. Expect the warmest, shortest-running mode
of the firmware - it belongs on the charger, like the radio.

## Gestures

| Gesture | In the movie mode |
| --- | --- |
| Tap | pause / continue |
| Swipe left | next movie |
| Swipe right | previous movie |
| Swipe up / down | volume, unchanged (global) |
| Long press (>= 500 ms) | next mode |
| Hold 5 s without moving | setup page |

The pause is cooperative: both tasks poll one flag, so the picture holds its
frame and the MP3 decoder keeps its position; the codec closes so the speaker
stays quiet. A paused movie draws a dimmed band with "Paused" and the file
name over the last picture. After the pause the picture continues from the
next frame, not from a seek.

## The alarm and other modes

The alarm owns the screen and the speaker while it rings. Both playback tasks
check `alarm_is_ringing()` and wait, so the ringing screen is not painted over
by the next frame and the beeps are not mixed into the sound track. Playback
continues where it stopped once the alarm is dismissed.

The picture task pushes straight to the panel, so the status row (web server
dot, IP address) is only visible in the paused overlay. The mode manager still
calls `movie_redraw()` when the status changes; while a picture plays there is
nothing to redraw.

## Settings, config and web form

- `MOVIE_ENABLE` in `features.h` (default 1, `0` removes the mode from the
  cycle). The module is compiled either way, so CI keeps checking it; a card
  without movies shows a hint, so the switch costs nothing.
- `MOVIE_DIR "/movies"` in `config.h.example` plus an `#ifndef` fallback in
  `settings.c`.
- `settings_t` grows `char movie_dir[32]` (appended last, so the blob migration
  keeps working), `SETTINGS_VERSION` 7 -> 8, `sanitize()` falls back to
  `MOVIE_DIR` when the value is empty.
- Setup page: one more field, `FIELDS` 30 -> 31.

## Risks

1. **Component vs IDF 6.1**: `esp_h264` 1.4.x is built for IDF >= 4.4 and is
   the same component Espressif ships for the S3; the movie mode is the first
   user of it in this firmware. The build is the check.
2. **Decode speed**: 240x240 is below the benchmark resolution, but a stream
   with many slices or a very high bitrate costs more. The script's fixed
   15 fps and CRF keep the stream in the tested range.
3. **Audio/video drift**: two clocks, one per task. Over a two-minute movie the
   drift is far below what the eye notices; seeking would expose it, which is
   why there is none.
4. **Pause race**: the playback task may be in the middle of a push when the
   pause lands. The mode waits 150 ms before it draws the band, so the band is
   always the last thing on the panel.
5. **Card speed**: 15 fps at 240x240 needs well under 1 MB/s, which every card
   manages; the script keeps the bitrate low on purpose.
6. **Container overhead**: about 15 % more bytes than the raw streams, because
   every PES ends in stuffing. The demuxer itself is one memcpy per packet, far
   below 1 % of the CPU at 25-50 kB/s.
7. **File size of the demuxer**: only what ffmpeg writes is understood - one
   program, H.264 in Annex-B, at most one MPEG audio stream, 188 byte packets.
   A file from another muxer with several programs or 192 byte packets is
   refused, not played wrong.

## Later

- Seeking (a progress bar and a tap position) once the MP3 track is constant
  bitrate and the frame index is in the header.
- Auto-advance through the folder as an alternative to looping.
- A still image mode for a single frame of a movie.
- H.264 from the network, sharing the radio's ring buffer.
