# Development

Developer notes for the moonstation firmware. `README.md` has the user-facing
overview, `docs/radio.md` the radio plan and its open points.

## Layout

```
moonstation/
├── platformio.ini
├── partitions_16mb.csv
├── config.h.example
├── config.h                 (tracked, no secrets, local board settings)
├── src/
│   ├── main.c
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   ├── config/              features.h, settings.c/.h, version.h
│   ├── core/                display, sdcard, touch, audio, battery, net,
│   │                        power, status, pins.h
│   ├── clock/               clock.c/.h
│   ├── alarm/               alarm.c/.h
│   ├── ui/                  draw.c/.h, font.h (generated)
│   ├── slideshow/           slideshow.c/.h (mode manager)
│   ├── weather/             weather.c/.h, weather_icons.c/.h
│   ├── radio/               radio.c/.h, stream.c/.h, pls.c/.h
│   └── web/                 webserver.c/.h, page.html/.css/.js,
│                            setup.html/.js, pages.h + certs.h (generated)
├── scripts/                 build_slides.sh, generate_font.py,
│                            generate_cert.sh, generate_web.py,
│                            pio_generate_web.py, fonts/
└── docs/                    development.md, radio.md
```

## Prerequisites

- PlatformIO (use the bundled venv binary)
- Python 3 with Pillow (font regeneration)
- ImageMagick 7 (`magick`) for preparing slide images
- OpenSSL (the build generates the self-signed certificate of the setup page)

## Build, flash, monitor

```bash
~/.platformio/penv/bin/pio run
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/ttyACM0
~/.platformio/penv/bin/pio device monitor
```

Serial capture without the monitor:

```bash
stty -F /dev/ttyACM0 115200 raw -echo && timeout 20 cat /dev/ttyACM0
```

That only shows traffic while the port is open, so it misses everything printed
before. To capture a boot, open the port and reset the chip over the DTR/RTS
lines of the USB-Serial-JTAG first:

```bash
~/.platformio/penv/bin/python - <<'EOF'
import serial, time
s = serial.Serial("/dev/ttyACM0", 115200, timeout=0.5)
s.dtr = False; s.rts = True;  time.sleep(0.1)
s.dtr = True;  s.rts = False; time.sleep(0.05)
s.dtr = False; s.rts = False
while True:
    print(s.readline().decode(errors="replace"), end="")
EOF
```

## Configuration

Copy `config.h.example` to `config.h` and adjust. `config.h` is tracked, so it
must not contain a credential: WiFi, the setup login and the weather key are
typed in on the setup page and live in NVS. Add new settings as a macro in the
example plus an `#ifndef` fallback in `src/config/settings.c`.

`config.h` seeds the defaults of `settings.c`, the only file including it. The
macros land in `settings_t`, NVS overlays stored values on top (`settings_init()`
at boot, `settings_save()` after a change), and the modules read the struct.
Without NVS content the firmware behaves exactly like the macros say; the web UI
only adds a way to change the subset that makes sense at runtime. The table in `README.md` lists the user-facing macros with their
defaults; the ones only in the template are `WEATHER_API_KEY` (empty, a key never
belongs in the README) and the build switches `BATTERY_SAMPLE_COUNT`,
`SCREENSHOT_ENABLE` and `SCREENSHOT_SERIAL` (see `features.h`).

Optional WiFi settings enable NTP time sync for the clock mode. A box without
them opens its setup access point on the first boot (see *First setup*).

## Deep sleep in clock mode

`CLOCK_DEEP_SLEEP` makes the clock mode deep sleep inside the local hours of
`CLOCK_DEEP_SLEEP_FROM`/`CLOCK_DEEP_SLEEP_TO` (default 23:00 to 07:00). The
compile switch builds the feature in and seeds the runtime setting of the same
name, which the setup page flips; the window hours are runtime settings too. The
display stays dark for the whole window, and a touch lights the clock up for the
normal dimming period. Deep sleep reboots the chip on every wake-up, so the
pieces fit together like this:

- Wake sources are the end of the window (timer) and touch (GPIO 12, ext0). The
  sleep entry refuses to run while the touch line is already low, so a held touch
  cannot loop the boot.
- The sleep check runs before the redraw and the backlight schedule. A wake-up
  that finds the window still active goes straight back to sleep - dark from the
  first millisecond (`main.c` configures the LEDC channel with
  `display_backlight_init()` instead of lighting it up), so no wake-up can flash
  the display at night.
- `display_deep_sleep_prepare()` blanks the backlight, puts the GC9A01 into sleep
  mode and latches the SPI pads (plus the PA pad in `audio_init()`) with GPIO
  holds; without them the pads float and feed noise into the chips. There is no
  PWM in deep sleep, so the backlight can only be fully on or off.
- On the next boot, `display_init()` and `audio_init()` release the holds and
  reinitialize panel and codec from scratch.
- `clock_init()` keeps the RTC time instead of resetting it to the compile time
  and skips WiFi/NTP while the window lasts. The wake at its end syncs again; the
  RTC has no 32.768 kHz crystal and drifts by seconds per day.
- A pending alarm shortens the sleep to the alarm time (`alarm_next_wake_us()`):
  the wake-up reboots, the alarm screen lights up, and the next idle pass sends
  the box back to sleep until the window ends.
- Window and displayed time use the local zone from `TIMEZONE_STD_OFFSET_HOURS`
  and `TIMEZONE_EU_DST`, applied as a POSIX TZ string in `apply_timezone()`
  before the compile time is interpreted.

## Power management

The build runs the CPU at 80 MHz with dynamic frequency scaling and tickless
idle (`CONFIG_PM_ENABLE`), so both cores idling drops to 10 MHz on their own:
`PM_MIN_CPU_MHZ` (`features.h`, default 10) is the lowest step outside light
sleep (40 MHz crystal / 4). The frequency only stays at 80 while a task runs or a
driver holds an APB lock; the power manager takes the minimum as soon as every
core is in the idle task.

`core/power.c` owns the policy: `power_init()` applies a profile at boot, and
modules request and release profiles instead of calling `esp_pm_configure()`
themselves. Requests are refcounted and the highest wins, so the setup page can
raise the ceiling while the slideshow keeps its floor:

- **`POWER_PROFILE_ACTIVE`** (always held): 80 MHz ceiling, `PM_MIN_CPU_MHZ`
  floor.
- **`POWER_PROFILE_BOOST`** (the setup web server and the radio while they run):
  240 MHz ceiling plus an `ESP_PM_CPU_FREQ_MAX` lock - TLS is the only workload
  that gains noticeably from it, and MP3 decoding next to a 40 MHz display SPI
  needs the headroom. Every holder releases it on its failure paths too.

Two more switches talk to the power manager:

- **`SLIDE_PM_LOW_POWER`** (default on) keeps the `CPU_FREQ_MAX` lock only around
  the image work: `show_slide()` and the SD card init/scan take it, the rest of
  the slide modes runs without it. Off, the slide modes hold the lock for their
  whole lifetime, which is what older versions did.
- **`PM_LIGHT_SLEEP`** (default off) additionally enables automatic light sleep,
  so the chip sleeps whenever no lock is held: between two images and between two
  clock minutes. The backlight PWM runs from `LEDC_USE_RC_FAST_CLK` with
  `LEDC_SLEEP_MODE_KEEP_ALIVE` and 11 bit duty, the only combination that
  survives light sleep on this chip (RC_FAST caps frequency x resolution, 5 kHz
  at 13 bit is impossible). The touch interrupt pad and the panel chip select are
  excluded from the GPIO power down, so a touch still wakes the chip and the
  panel does not latch noise.

Light sleep is off by default because this board is flashed and monitored over
the ESP32-S3 **USB Serial/JTAG** peripheral, which dies in light sleep and takes
the port with it (the kernel then logs `device not accepting address, error -71`
in a loop). The firmware keeps an `ESP_PM_NO_LIGHT_SLEEP` lock while
`usb_serial_jtag_is_connected()` reports a host, but that path is not proven on
hardware; and plugging the cable in later does not wake the chip, since USB is no
wake source - a board that slept unplugged needs the reset button. Enable the
switch for a board that runs unplugged and keep that button reachable.

## Display and backlight

The panel is a GC9A01 over SPI. The backlight on GPIO 42 is active low and PWM
controlled: `display_backlight_init` configures the LEDC timer/channel and leaves
the pad dark, `display_backlight_on` adds full brightness, off sets 0 %. The PWM
runs at 5 kHz with 11 bit duty from `RC_FAST`; the resolution follows from that
clock source (see above) and is plenty for a backlight.

Colors in the code are plain RGB565; the byte swap for the panel happens exactly
once per write path with `RGB565_SWAP()` from `display.h`: in the drawing helpers
of `ui/draw.c`, in `clock.c` where it blanks the shared buffer and in
`fill_color()`. The JPEG decoder needs no swap - it is configured with
`JPEG_PIXEL_FORMAT_RGB565_BE` and writes the most significant byte first on its
own. The swap is needed because the panel expects that byte order, esp_lcd
reverses only command and parameter bytes, and the CPU stores a `uint16_t` least
significant byte first. White and black are palindromic and unaffected, which is
why a missing swap only shows as blue instead of red and dark red instead of
green. Never put pixels into a frame buffer without going through one of those
paths.

## SD card and images

The card is mounted over SDMMC (1-bit). Images must be 240x240 baseline JPEGs;
`scripts/build_slides.sh` converts arbitrary images, then copy the output to the
card. Decoding uses `espressif/esp_new_jpeg` (SIMD accelerated on the ESP32-S3),
not the TJpgDec ROM codec of `espressif/esp_jpeg`: measured over 30 slides at
80 MHz, a 240x240 slide decodes in 36 ms on average (25-50 ms) instead of 189 ms
(156-231 ms), 5.3x faster. The frame buffer in `display.c` must stay 16-byte
aligned, which is what the SIMD path requires.

Decoder handle and JPEG input buffer are process-wide resources of the slide
modes: the buffer grows to the largest file and is reused for every image, so
thousands of image changes cannot fragment the heap, and both are released in the
mode's `exit` hook when the clock mode takes over. The clock mode therefore runs
with the memory of the old firmware (~114 kB free), the slide modes pay roughly
14 kB for the decoder.

### Crossfade

`SLIDE_FADE_MS` (default 0, hard cut) blends from the image on screen to the next
one. It needs a second frame: the new image is decoded into a 115.2 kB buffer in
PSRAM while the panel keeps the old one, then the two are mixed in 16-line bands
into a small internal buffer and pushed, so the transition needs no third buffer.
The build enables the 8 MB octal PSRAM for this (`CONFIG_SPIRAM`,
`sdkconfig.defaults`); without it the setting is ignored and the image is drawn
in one go.

One step costs about 57 ms at 80 MHz: the SPI transfer of a full frame (115 kB at
40 MHz) plus the blend. The step count therefore comes from
`SLIDE_FADE_MS / 57 ms`, and 500 ms measures 459 ms over 9 steps. Decode time is
unaffected (36-49 ms), the extra internal RAM is the 7.7 kB band buffer, the
second frame is 115.2 kB of PSRAM. A fade writes ~1.8 MB to the panel, so it is
off by default for battery operation.

In clock+slideshow mode the clock is part of both frames: `clock_compose()` draws
the overlay into the new image before the fade starts, so unchanged pixels are
identical in both frames and `blend565(a, a, alpha)` leaves them untouched -
only the slide crossfades. The same mode keeps a third PSRAM buffer with the
clock-free slide (`clean_buf`), because the overlay paints over the slide pixels
in the shared frame buffer; restoring the copy on a minute change avoids
re-reading and re-decoding the JPEG every minute. Without PSRAM the mode falls
back to decoding again.

PSRAM is part of the default heap after boot, which is why
`esp_get_free_heap_size()` reports ~8.3 MB in the logs. The internal-only figure
is `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`.

## Touch

CST816D on I2C port 1. Touch is interrupt driven: the controller's INT line
(GPIO 12, falling edge) wakes a dedicated task that reads it over I2C, so the
normal path costs two wake-ups per interaction. Taps, swipes and long presses
fire on release: a tap is a hold under 0.5 s that did not move, a longer one at
the same spot switches the mode, and a touch that moved at least
`TOUCH_SWIPE_MIN_PX` (25 px) is a swipe - however long it took. The time is not
part of that decision on purpose: a careful volume slide is slow, and a time
limit turned it into a mode switch in the middle of a station. Deferring all
three keeps a deliberate hold free for the setup gesture, where the finger has to
stay at one spot for 5 s. If no release interrupt arrives, the task falls back to
100 ms polling until the finger is lifted; the same poll runs from the moment a
finger lands, so a fast flick is sampled even if the controller only reports its
landing point. A swipe never also fires a tap or a mode switch and cancels the
setup gesture like any other movement. Touch is optional: missing hardware is
logged and ignored.

## Audio

ES8311 on I2C port 0 plus I2S. A tone is opened per call and the codec closed
afterwards, so the PA stays off when idle; `audio_play_tone(freq, ms)` generates a
sine wave at 24 kHz.

A decoder can keep the codec open instead. `audio_stream_begin(rate)` opens it
once at the stream's rate - the I2S data interface reconfigures its clock, so
44.1 and 48 kHz work next to the 24 kHz tones - `audio_stream_write(pcm, bytes)`
blocks until the DMA has room and therefore paces the decode loop, and
`audio_stream_end()` closes it again. PCM is interleaved 16 bit stereo.

One mutex guards the codec, because tones come from the touch task and the mode
manager while a stream comes from the decoder task. A tone that arrives while a
stream is open is written into the same codec at the stream's rate instead of
re-opening it, so an alarm neither clicks nor changes the rate under the decoder;
the decoder waits on the lock and continues afterwards. The tone is generated in
blocks of 512 samples on the heap: the touch task has a 3 kB stack and must not
build a long tone in one piece.

## Battery

`core/battery.c` reads the charge level from the voltage divider on GPIO 1 (ADC1
channel 0, 12 dB attenuation, 12-bit) and the charging input on GPIO 41 (internal
pull-up, high while charging). Raw readings are averaged over three samples and
mapped to 0-100 % with a per-board lookup table taken from the vendor firmware,
so no ADC calibration config is needed.

A repeating `esp_timer` samples every `BATTERY_REFRESH_SEC` seconds and writes one
log line per refresh with the raw ADC value, so a wrong percentage can be traced
back to the sensor. The display only reads that cache and never starts a
conversion itself. At `BATTERY_WARN_PCT` or less a short quiet beep sounds
(`BATTERY_BEEP_VOLUME`). The monitor is initialized from `clock_init()`.

## UI modes

The main loop in `slideshow.c` is a mode manager. Every mode implements
`ui_mode_t`: `enter`/`exit` for transitions, `on_tap(x, y)`/`on_long_press`/
`on_swipe` for touch events (they run in the touch task), `on_wake` for periodic
work, and `next_wake_us` telling the manager when to wake it.

The modes are `clock`, `slideshow`, `combined` (clock + slideshow), `weather`
(`WEATHER_ENABLE`) and `radio` (`RADIO_ENABLE`). A long press cycles through them
in that order and the box boots into clock mode; a switch without the switch
compiled in is skipped. `enter()` always redraws the full initial state, so a
mode switch is visible at once, not on the next slide or minute. A long press
switches when the finger is lifted.

The weather mode is the reference for a mode with its own module: the `ui_mode_t`
instance in `slideshow.c` only forwards `enter`, `on_wake` and the next wake-up
time to `weather.c`, plus the `next` link in the cycle. Another mode works the
same way: one struct, linked in with `next`.

## Volume gesture and overlay

A vertical swipe changes the output volume in 5 % steps in every mode, up being
louder. The travel decides how many steps one gesture is worth: one step per
`VOLUME_STEP_PX` (40 px), at least one and at most `VOLUME_MAX_STEPS` (5). A
slide across most of the panel is therefore a quarter of the range, and a short
flick still nudges it. The gesture lives in the mode manager: the touch callback
in `slideshow.c` only counts steps and wakes the main loop, which applies them to
`settings.audio_volume`, pushes the value to `audio.c`
(`audio_set_volume()` works on an open codec, so a running stream follows) and
writes the settings back once per gesture. Horizontal swipes belong to the mode -
the radio changes station with them; a mode without a swipe callback ignores
them. Modes that draw their own buttons ask for steps with
`slideshow_volume_steps()` and land in the same path.

Two rules keep it out of the way: while the alarm rings the gesture is ignored,
because the alarm enforces `ALARM_MIN_VOLUME_PCT` and a lower setting would only
make the bar lie; and a swipe counts as an interaction, so it lights the display
at full brightness.

The bar itself is `src/ui/osd.c`, an overlay that works in every mode without the
mode knowing about it:

- The covered rectangle (152x64 px in the middle) is copied into PSRAM, the bar
  is drawn into the shared frame buffer and only that rectangle is pushed.
- After 1.5 s the copy is put back. A checksum of the rectangle as it was drawn
  decides whether that is still safe: if a mode painted over the overlay in the
  meantime, the restore is dropped instead of putting old pixels over newer ones.
- The deadline feeds the mode manager's wake-up calculation
  (`osd_next_wake_us()`), so no timer of its own is needed.

Everything here runs in the main task; the touch task only leaves the pending
steps behind, the same rule the modes follow for the display.

## Alarm

A daily alarm at one local time. `settings_t` holds `alarm_enabled`, `alarm_hour`
and `alarm_minute`; the setup page edits them as one time field. `alarm.c` owns
the rest:

- **Detection**: `alarm_on_wake()` runs first in every pass of the mode manager
  loop and compares the local wall clock with the configured time. It accepts a
  ring up to 60 s late, so a pass busy with a JPEG decode, a weather request or
  the boot after a deep sleep wake-up still starts it, and it remembers the day
  of the last ring (`last_rang`), so it cannot start twice before midnight.
- **Wake-up**: `alarm_next_wake_us()` returns the time to the next occurrence (or
  one second while it rings). The mode manager takes the minimum of that and the
  mode's own wake-up, and the deep sleep check clamps the sleep to an alarm
  inside the night window.
- **Ringing**: the alarm paints its own screen - the clock face plus a blinking
  `ALARM` label and a `tap to stop` hint - so it looks the same in every mode.
  The mode's `on_wake()` is skipped while it rings, the backlight stays at full
  brightness, and the main task plays two rising tones every two seconds. The
  beeps use at least 70 % volume, so a box whose feedback volume is turned down
  still rings. A tap or a long press calls `alarm_dismiss()`; the auto-stop
  follows after five minutes.
- **Threads**: `alarm_on_wake()` and the beeps run in the main task,
  `alarm_dismiss()` runs in the touch task and only clears the volatile flag, so
  the codec is never driven by two tasks.

## Weather mode

`WEATHER_ENABLE` in `features.h` (on by default, `0` removes the step) puts a
fourth step into the cycle. The module is compiled either way, so CI keeps
checking it; the switch only decides whether the mode joins the cycle and calls
`weather_init()` at boot. Without a stored API key the mode still enters but
shows a hint that points at the setup page, so leaving it on costs nothing.

- **Data source**: the free Current Weather 2.5 plus the 5 day / 3 hour forecast,
  both by coordinate (`lat`/`lon`; the city-name query is deprecated) in metric
  units. The forecast request asks for `cnt=40`, the whole five days: the first
  eight steps feed the 24 hour page, and all of them are aggregated per local
  calendar day (min/max, highest rain chance, the condition closest to noon). A
  refresh is two calls; at 20 minutes that is far below the free tier of 1000
  calls per day. One Call 3.0 needs its own subscription and a credit card, the
  16 day endpoint is a paid plan.
- **Pages**: the current conditions show the condition icon, the temperature in
  the big clock font, the description, the felt temperature, high/low and
  humidity/wind. A tap switches to the forecast page: eight columns of 3 hour
  steps with time, icon and temperature, plus the highest chance of rain in the
  header. Another tap shows five rows of weekday, icon, high and low, with the
  rain chance where it is at least 20 percent. A third tap goes back; `enter()`
  restarts on the current page.
- **Refresh**: `weather_next_refresh_us()` asks the mode manager to wake the main
  task when the reading is `WEATHER_REFRESH_MIN` minutes old. A failed request
  (no WiFi, bad key, API down) is retried after 5 minutes instead of hammering
  the API. A reading older than two intervals gets a small amber ring at the top
  edge; without a reading or a key the screen shows a hint.
- **Cache**: the last successful answer lives as one versioned blob in the NVS
  namespace `weather` (key `cache`), so a reboot without WiFi still has the last
  screen. The request needs `net_wifi_up()` and releases the radio with
  `net_wifi_down()`, which respects the setup server's hold.
- **Threads**: `on_tap` runs in the touch task and only sets a flag; `on_wake`
  does the request and the redraw in the main task. The request URL contains the
  API key (`appid`), so nothing in the module logs a URL - only the endpoint and
  the answer.
- **Rendering**: the icons in `weather_icons.c` are drawn from the primitives of
  `ui/draw.c` at whatever size the layout wants; the big temperature uses the
  fixed-cell clock font, so it lines up with the clock mode, and the degree mark
  is a small ring after the digits. Body text uses the proportional font.

## Radio mode

`RADIO_ENABLE` in `features.h` (on by default, `0` removes the step) puts a fifth
step into the cycle. The module is compiled either way, so CI keeps checking it;
without playlists the mode still enters and shows a hint. `docs/radio.md` has the
plan, the measurements behind the two-task design and the open points. In short:

- **Stations**: `radio_enter()` mounts the card (it stays mounted), scans
  `RADIO_DIR` (default `/radio`) with `pls_scan()` and keeps the result in a PSRAM
  array. One `.pls` file is one station, sorted by file name, up to 32. `File1`
  plays, `File2..N` are mirrors of the same stream; only absolute `http://`
  addresses are used. The screen name is the file name without its extension,
  `Title1` the second, dimmer line.
- **Playback** (`stream.c`): two tasks share a 64 kB ring in PSRAM. The network
  task reads 4 kB blocks and may block as long as the server needs; the decoder
  takes frames out of the ring and writes PCM through the codec stream API. The
  split is not cosmetic: `esp_http_client_read()` returns only after the **full**
  requested length, so a read inside the decode loop starves the I2S DMA (one
  task, 48 kB reads: 7.4 s of audio per 12 s of wall clock, a gap every three
  seconds). A 1 s read timeout bounds how long `stream_stop()` waits, and 10 s
  without a byte counts as a dead station.
- **States**: `Connecting -> Buffering -> Playing`, plus `Paused` and `No signal`.
  Playback starts once 24 kB are buffered (~1.2 s at 160 kbit/s). The screen is
  redrawn on a change only; `radio_next_wake_us()` asks for a 200 ms wake while
  connecting or buffering and 2 s while playing, so a stream that dies silently
  still shows up quickly.
- **Gestures**: the mode draws its own play/pause button in the middle, and only
  a tap inside it (38 px radius) plays or pauses. Every other tap changes the
  volume - above the button louder, below it quieter, two steps each - through
  `slideshow_volume_steps()`, so bar and NVS write stay in the mode manager. The
  split exists because the controller reports some slides as a tap; with
  tap-anywhere as pause the station stopped itself during a volume gesture.
  Pausing really stops the stream and closes the codec (the PA stays off);
  resuming reconnects, which is what a live stream wants and keeps the code free
  of a paused-network state. A horizontal swipe changes the station and wraps
  around, vertical swipes stay the volume gesture.
- **Failures**: 2 s, 5 s, 10 s and then every 10 s, rotating through the mirrors.
  A tap on the button retries immediately. Without WiFi the producer reports
  "No WiFi" and the next attempt tries `net_wifi_up()` again, so the mode recovers
  by itself.
- **Power**: the stream tasks take `POWER_PROFILE_BOOST` and `net_wifi_hold()`
  while they run and release both when they end - a paused or failed radio does
  not keep the CPU at 240 MHz or the WiFi out of power save.
- **Settings**: `RADIO_DIR` with its fallback in `settings.c`,
  `settings.radio_dir`, `SETTINGS_VERSION` 7 and one field on the setup page.

## Setup mode and status dot

A 5 s hold without drifting (`TOUCH_SETUP_MOVE_PX` in `core/touch.c`) is the
setup gesture: `slideshow.c` acknowledges it with a 120 ms beep and calls
`web_toggle()`, which starts the setup server or stops it again. While it
listens, `ui_status_draw()` paints a red dot in the middle of the lower edge -
the round panel cuts off the corners, so the dot sits a little above the bottom -
and `clock.c` shows the page address in the row the battery indicator uses.
`core/status.c` exists so the UI only has to read flags: the web server writes
them, clock and slideshow draw them, and neither includes the other.

## Setup web server

`src/web/webserver.c` is deliberately isolated: it talks to `core/net.c` for the
radio, to `settings` for the values and to `status` for what the screen shows.
Nothing else in the firmware knows it exists.

- **Transport**: HTTPS on port 443 (`esp_https_server`). Certificate and key are
  self-signed, generated at configure time by `scripts/generate_cert.sh` into
  `web/certs.h` (git-ignored), so no private key lands in the repository. The
  browser warning is expected for a device without a domain.
- **Pages**: markup, stylesheet and scripts are plain files under `src/web/`
  (`page.html/.css/.js` for the settings form, `setup.html/.js` for the first
  login form). The HTML files pull the others in with an `<!-- include: name -->`
  line, `scripts/generate_web.py` substitutes them verbatim and writes
  `web/pages.h` before every build. Each page is therefore one document in flash,
  served with a single `httpd_resp_send()` from the only route, `/`, with
  `Cache-Control: no-store`. A missing file fails the build.
- **Sockets**: a TLS socket costs about 40 kB of RAM, so the pool holds two; if
  the heap cannot take the second one, the start retries with one.
- **Auth**: HTTP Basic, checked against a header built once at start. The login is
  never empty: a missing or cleared password falls back to the factory
  `WEB_USER`/`WEB_PASSWORD` (`admin`/`admin`), see *First setup*.
- **API**: `GET /api/config` returns the editable subset as form encoded
  `key=value` pairs, `POST /api/config` applies and stores it, `POST /api/reset`
  stores the build time defaults again while keeping WiFi credentials, weather
  key and setup login, and `POST /api/shutdown` closes the page. Every field also
  carries its default as `key.def`, which the page shows next to the label; the
  `min`/`max` limits sit in the markup and mirror the clamp ranges of `FIELDS`.
  `URLSearchParams` on both sides reads and writes that format, so quotes, spaces
  and umlauts in passwords survive without an escaping bug.
- **Build stamp**: `GET /api/config` also returns `fw_version` and `fw_built`,
  the two macros from `src/config/version.h`, and the page shows them in its
  footer. The date and time are the compiler's `__DATE__`/`__TIME__`, so they are
  exactly the build of the binary answering; `FW_VERSION` is bumped by hand with
  the release tag. The same line is logged at boot (`moonstation boot, firmware
  0.5.0 built ...`), which usually tells two builds apart from a serial capture.
- **Secrets**: `wifi_password`, `web_password` and `weather_api_key` carry the
  `secret` flag in `FIELDS` and are write-only. `GET /api/config` reports
  `<key>.set=0|1` instead of the value and no default (that would be the template
  from the config file). A POST with an empty secret field keeps what is stored,
  so the page can be saved without retyping the key. `<key>.clear=1` erases the
  WiFi password and the weather key; the setup password cannot be erased, because
  the page is never left open. No credential has a placeholder in the config
  template; the setup page is the only way to set one.
- **Layout**: the form is grouped into `fieldset`s with legends (Network, Image
  show, Backlight, Clock and time, Alarm, Weather, Sound and battery, Setup
  access). `page.js` walks `form.elements`, so the grouping cannot affect loading
  or saving, and a new field only needs its markup plus an entry in `FIELDS`. The
  exception is the alarm time: the page has a single `<input type=time>` while
  the firmware stores `alarm_hour` and `alarm_minute`, so `page.js` splits and
  joins that pair.
- **Radio**: on the home network the server holds the link with `net_wifi_hold()`
  for as long as it listens, so the daily NTP sync cannot take the page down.
  Without a network it serves the page over the setup access point, which keeps
  the radio up by itself.
- **Lifetime**: `WEB_IDLE_TIMEOUT_MIN` minutes without a request stop it, so a
  forgotten session does not keep the radio up all night.

### First setup

`start_server()` tries the station link first. When there are no credentials or
the network does not answer, it calls `net_ap_start()` and serves the same page
over an open access point:

- SSID `Moonstation-XXXX`, `XXXX` from the last two bytes of the soft AP MAC,
  channel 6, address 192.168.4.1; the default DHCP server of the AP interface
  hands out the leases.
- `net_ap_start()` runs `wifi_stack_init()`, which creates the station netif, the
  AP netif and the driver exactly once; `esp_wifi_set_mode(WIFI_MODE_AP)` then
  replaces the station mode. `net_ap_stop()` stops the radio when the page closes.
- While the login is still the factory one, `/` serves `setup_html` instead of the
  settings form, and `POST /api/config` accepts only `web_user` and
  `web_password`. An empty or unchanged factory password is rejected with 400,
  `/api/reset` with 403. Saving a new login rebuilds the Basic auth header
  immediately, so the next request asks for it.
- Saving WiFi credentials while the page runs on the AP is answered first;
  `esp_restart()` follows two seconds later and the next boot joins the network.
- On boot without credentials in NVS, `slideshow_run()` calls `web_start()` by
  itself, so the first setup needs nothing but a phone. The 5 s gesture uses the
  same fallback when the home network cannot be reached.
- The AP is open on purpose: it serves only the login form, the factory login has
  to be replaced before anything else appears, and the AP disappears once WiFi is
  configured.

## Clock mode

The clock shows HH:MM and the date, with a small per-minute jitter to reduce
burn-in. Time comes from the compile time by default; WiFi/NTP can be enabled in
`config.h`. The radio and the SNTP stack live in `core/net.c` and are always
linked, so firmware built without credentials keeps the compile time and runs
only the setup access point on the first boot.

`__DATE__`/`__TIME__` are read as local time: the TZ string is installed first
and `mktime()` resolves summer time, so only the build machine's clock error
remains in the fallback. With credentials the firmware waits for the real SNTP
answer instead of guessing from a valid-looking system time.

The boot sync is followed by a daily one at `NTP_DAILY_SYNC_HOUR`, armed with a
one-shot `esp_timer` and run in its own task, because the radio/NTP cycle takes
seconds the timer task must not spend blocked. The WiFi setup (netif, event loop,
driver) runs once per boot, only start/connect/stop repeat, so a second station
interface can never be created.

Below the weekday sits the battery row: a small icon filled to the charge level,
the percentage next to it and an upward arrow while charging. It is white
normally, green while charging and red at or below `BATTERY_WARN_PCT`, and moves
with the same jitter as the date. `CLOCK_SHOW_BATTERY` `0` hides it.

The clock is timer driven: the screen is redrawn only when the displayed minute
changes, aligned to the minute boundary. A one-shot `esp_timer` (`ui_wake`) wakes
the main loop at the next slide, minute boundary or backlight step, and touch
callbacks wake it via a task notification, so there is no polling redraw. In
combined mode the overlay is refreshed the same way.

## Fonts

The firmware carries two bitmap fonts, both generated into `src/ui/font.h` from
the TTFs in `scripts/fonts`:

- **`font_time`**: DejaVu Sans Bold 48, the big clock digits. Fixed 35x52 cells,
  one bit per pixel, only `0-9`, `:` and `-`; used by the clock and the weather
  temperature.
- **`font_text`**: Roboto Medium 16, the system font family of Android.
  Proportional and antialiased with 8 bit alpha, ASCII 32-126 plus the degree
  sign at 0xB0. Line height 19 (ascent 15, descent 4), cap height 12. Each glyph
  stores its ink box, the offset from the pen position and its advance, so
  `draw_text()` lays a line out like a normal renderer and `draw_text_width()`
  returns the exact width.

The antialiasing blends every glyph pixel with the destination pixel
(`blend_pixel()` in `ui/draw.c`), so text always goes on top of an already filled
buffer; drawing into uninitialized memory would blend with garbage. Regenerate the
header after a font or TTF change (requires Pillow) and commit it:

```bash
python3 scripts/generate_font.py
```

Roboto is Apache 2.0 licensed, the license text sits next to the TTF in
`scripts/fonts/`.

## Continuous integration

`.github/workflows/ci.yml` runs on pushes to `main` and on pull requests:

- **Static analysis**: cppcheck over `src` (warnings, performance and portability
  checks, any finding fails the run).
- **Build firmware**: `pio run` on a clean runner with `config.h` created from
  `config.h.example`. `firmware.bin`, `bootloader.bin` and `partitions.bin` are
  uploaded as a workflow artifact.

Both workflows pin the PlatformIO Core version (currently `6.2.0`) and keep two
separate caches, because they age at very different rates:

| Cache | Contents | Key |
| --- | --- | --- |
| `pio-toolchain-*` | `~/.platformio/packages`, `~/.platformio/platforms` | platform pin from `platformio.ini` |
| `pio-pip-*` | pip wheel cache | PlatformIO Core version |

The toolchain cache is the expensive one (about 1.3 GB), so it must not be
invalidated by unrelated edits: a change to `src/idf_component.yml` or
`dependencies.lock` only touches `managed_components/` inside the project, so
those files are deliberately kept out of the key. The download archives in
`~/.platformio/.cache` are not cached either; they are already compressed and
double the archive size without helping a cache hit. Bump `CACHE_VERSION` in
both workflow files when the cache layout changes.

`.github/workflows/release.yml` runs on version tags (`v*`), builds the same way,
merges the images into a single flashable binary and publishes a GitHub Release
with all binaries attached:

```bash
git tag v0.1.0
git push origin v0.1.0
```

## Debugging

Use the serial monitor at 115200 baud. Build artifacts and logs are under
`.pio/`. Check the init results for display, SD card, touch and audio on boot.

## Conventions

- One logical change per commit; short English commit messages
- Keep `README.md`, `AGENTS.md` and `PLAN.md` in sync with the code
- Never commit a credential, `*.bin` or `.pio/`
- Leave binary and data assets (including slide images) untouched
