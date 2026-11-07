# Dual boot: moonstation and xiaozhi-esp32

Plan for keeping both firmwares in the 16 MB flash of the Spotpear
ESP32-S3 1.28" BOX and switching between them at runtime, the way the
Espressif blog post "How to switch between multiple ESP32 firmware binaries"
describes: one application in `factory`, the other in an OTA partition, and
`esp_ota_set_boot_partition()` plus a restart to hop between them.

Status: implemented and tested in both directions on the box.

Verified on the box:

- the new table writes and boots (bootloader log lists all seven partitions),
  `factory` is chosen with `otadata` empty, NVS survived (settings load, WiFi
  joins), SD card, display, touch and audio work.
- xiaozhi v2.5.3 boots from `ota_0` under moonstation's IDF 6.1 bootloader:
  8 MB PSRAM at 80 MHz, GC9A01, backlight, ES8311 and LVGL all come up, and it
  starts its own `Xiaozhi-XXXX` WiFi setup AP.
- a two second hold on the BOOT button (GPIO 0) switches in both directions:
  from moonstation into `ota_0` (`bootswitch: next boot: xiaozhi 2.5.3` in the
  log), and from xiaozhi back to `factory`, where the bootloader loads
  moonstation 0.5.0 with its settings, SD card, touch and audio intact.
- the setup-page button and the USB route (`erase_region 0xe000 0x2000`) reach
  the same state.

The touch gestures of both firmwares are untouched: no hold on the screen
switches firmware.

Still open: the "Start xiaozhi" button on the setup page has not been pressed on
a real browser yet (it is the same call the BOOT hold makes), and a full round
trip after a moonstation reflash or a power cycle is not run yet.

## Verdict

Space is not the problem. The flash is 16 MB and both firmwares together use
about 5.4 MB:

| Image | Bytes | MiB |
| --- | --- | --- |
| moonstation app (current build) | 1,527,456 | 1.46 |
| xiaozhi-esp32 app (`sp-esp32-s3-1.28-box`, v2.5.3) | 2,692,912 | 2.57 |
| xiaozhi assets image (wake words, fonts, sounds) | 1,262,657 | 1.20 |
| bootloader, partition table, NVS, otadata, PHY | ~250,000 | 0.24 |
| **total** | **~5.7 MB** | **~5.5** |

The current layout wastes 12 MB on an unused `spiffs` partition (nothing in
`src/` references SPIFFS; slides, movies and radio live on the SD card), so
that space is simply repurposed.

The real work is not size, it is:

1. one shared partition table and **one shared bootloader**,
2. never letting xiaozhi's build overwrite the bootloader or the table,
3. a way back from xiaozhi to moonstation,
4. shared NVS between two firmwares that both think they own it.

## Board support is already there

xiaozhi-esp32 ships a board for exactly this device:
`main/boards/spotpear/sp-esp32-s3-1.28-box` (pins match ours: GC9A01 on
2/4/5/47/38, backlight 42 inverted, CST816D on 11/7/6/12, ES8311 on
16/9/45/10/8/46). No porting is needed, only a rebuild for our partition
layout.

The local checkout is the fork next to this project (2.5.3,
`v2.5.2-135-g50e96be`, branch `feature/moonstation-dual-boot`). Its `build/` was
produced with an ESP-IDF in `/opt/esp-idf` that no longer exists, so it was
rebuilt against the IDF 6.1.0 tree that PlatformIO already carries; both
firmwares then come from the same IDF and the same bootloader generation.

Build recipe (nothing has to be installed system-wide, the IDF tree and the
compiler come from `~/.platformio/packages`):

```bash
python3 -m venv /tmp/idf61-venv
/tmp/idf61-venv/bin/pip install -r \
  ~/.platformio/packages/framework-espidf/tools/requirements/requirements.core.txt

export IDF_PATH=$HOME/.platformio/packages/framework-espidf
export IDF_PYTHON_ENV_PATH=/tmp/idf61-venv
export ESP_IDF_VERSION=6.1.0
export PATH=$HOME/.platformio/packages/toolchain-xtensa-esp-elf/bin:$PATH
/tmp/idf61-venv/bin/python $IDF_PATH/tools/idf.py build
```

`/tmp/idf61-venv` is throw-away; the pinned versions of `click`, `cryptography`,
`pyparsing` and `esp-idf-nvs-partition-gen` in `espidf.constraints.v6.0.txt`
have to win over the newest ones, otherwise `idf.py` refuses to start. The
component manager needs `ESP_IDF_VERSION` when `idf.py` runs outside an
`export.sh`; all components are already in `managed_components/`.

What is flashed from that build is the app and the assets only:

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash \
  0x310000 build/xiaozhi.bin 0xa10000 build/generated_assets.bin
```

## Updating either firmware later

**moonstation** — unchanged from before:

```bash
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/ttyACM0
```

That writes bootloader (0x0), partition table (0x8000) and the app (0x10000)
and touches neither `otadata` nor the xiaozhi slots. If `otadata` currently
selects `ota_0`, the box keeps starting xiaozhi after the upload; a 2 s BOOT
hold still comes back to `factory`, or clear `otadata` over USB
(`esptool.py erase_region 0xe000 0x2000`). A build that no longer fits the
3 MiB `factory` slot fails on its own (`board_upload.maximum_size`).

**xiaozhi** — rebuild with the recipe above, then flash app and assets:

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash \
  0x310000 build/xiaozhi.bin 0xa10000 build/generated_assets.bin
```

Never `idf.py flash` and never a release `merged-binary.bin`: both write the
bootloader, the partition table and `ota_data_initial.bin`, which would replace
the shared layout and drop the BOOT-button switch. The assets only need the
second address when the file actually changed, but flashing both is harmless.
Its own OTA is switched off in the fork, so updates always come over USB.

If a future xiaozhi release grows past 3.5 MiB, enlarge `ota_0`/`ota_1` and
shrink `assets` in `partitions_16mb.csv`; both firmwares have to be flashed
again afterwards because the table changed.

## Proposed partition table

Replaces `partitions_16mb.csv`. Validated with `gen_esp32part.py`.

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
factory,  app,  factory, 0x10000,  0x300000,
ota_0,    app,  ota_0,   0x310000, 0x380000,
ota_1,    app,  ota_1,   0x690000, 0x380000,
assets,   data, spiffs,  0xa10000, 0x400000,
phy_init, data, phy,     0xe10000, 0x1000,
```

| Region | Offset | Size | Content |
| --- | --- | --- | --- |
| bootloader | 0x0 | 32 KB | shared, from moonstation |
| partition table | 0x8000 | 4 KB | this table |
| nvs | 0x9000 | 20 KB | shared by both firmwares |
| otadata | 0xe000 | 8 KB | boot selection |
| factory | 0x10000 | 3 MiB | moonstation (49% full) |
| ota_0 | 0x310000 | 3.5 MiB | xiaozhi (73% full) |
| ota_1 | 0x690000 | 3.5 MiB | xiaozhi OTA slot |
| assets | 0xa10000 | 4 MiB | xiaozhi assets (30% full) |
| phy_init | 0xe10000 | 4 KB | PHY calibration |
| free | 0xe11000 | 1.93 MiB | headroom |

Notes on the choices:

- `factory` stays at 0x10000, so `pio run -t upload` keeps writing
  bootloader + table + app at the same addresses as today and never touches
  the xiaozhi partitions.
- `nvs` keeps its current offset and size, so the WiFi credentials, weather
  key and setup login survive the migration.
- The old `phy_init` at 0xe000 is overwritten by `otadata`; PHY calibration
  data is regenerated on the next boot, nothing is lost.
- `ota_1` is optional. It keeps xiaozhi's own OTA working (and gives a spare
  slot). Dropping it frees another 3.5 MiB.
- `assets` is 4 MiB even though the current image is 1.2 MiB, to leave room
  for larger wake-word sets in later xiaozhi releases.

The graphical bootloader from the blog (`georgik/esp32-graphical-bootloader`)
would also fit (a third app of ~1 MiB in `factory`, the two firmwares in
`ota_0`/`ota_1`), but it would need a port to the GC9A01 panel and the
CST816D touch controller, and it adds a menu between every boot. Not worth it
unless a visible picker is wanted.

## Rules for the shared flash

These are the things that break a dual-boot setup if ignored.

1. **One bootloader, one table.** The 2nd stage bootloader comes from
   whichever project flashed last. Keep moonstation's (IDF 6.1, DIO 80 MHz,
   16 MB, `CONFIG_SPIRAM_BOOT_INIT=y`, octal PSRAM 80 MHz, app rollback
   disabled).
   - PSRAM is initialised in the bootloader, so a bootloader without the
     octal/80 MHz settings leaves both apps without PSRAM.
   - Rollback must stay disabled. With it enabled, a boot into `ota_0` is
     only "confirmed" once the app calls `esp_ota_mark_app_valid_cancel_rollback()`,
     and xiaozhi only does that after it reached its server, so an offline
     boot would silently fall back to moonstation.
2. **Never flash xiaozhi's bootloader, partition table or `ota_data_initial.bin`.**
   `idf.py flash` / the release `merged-binary.bin` write all three and would
   replace this layout. Only the app and the assets go to flash, with
   `esptool.py write_flash` at the offsets above.
3. **Same flash mode.** Both images are DIO at 80 MHz; both projects' flash
   arguments say so. Mismatched image headers are the classic way to get a
   firmware that flashes but does not boot.
4. **`board_upload.maximum_size` must drop** from 4 MiB to `0x300000`, so
   PlatformIO rejects a moonstation build that no longer fits `factory`.
5. **NVS is shared as one partition, but not as one config.** Both firmwares
   use the same `nvs` partition and the same `settings`/`wifi`/`weather`
   namespaces are free of collisions, but they do not reuse each other's WiFi
   credentials: moonstation lets the WiFi driver store them (`nvs.net80211`),
   xiaozhi disables that and keeps its own copy in its `wifi` namespace
   (log: `SsidManager: NVS namespace wifi doesn't exist` on its first boot).
   So WiFi is set up once per firmware. The shared partition is also the
   dangerous part: xiaozhi's system reset calls `nvs_flash_erase()` and wipes
   moonstation's `settings` and `weather` namespaces too. Patch that call to
   erase only its own namespaces, or accept re-provisioning after a xiaozhi
   reset.
6. **`pio run -t upload` does not clear `otadata`.** If the device currently
   boots `ota_0`, flashing moonstation leaves it booting xiaozhi. Clear
   `otadata` (`esptool.py erase_region 0xe000 0x2000`) or switch from inside
   the running firmware.

## Switching

### moonstation -> xiaozhi

Implemented:

- `src/core/bootswitch.c/.h`: `bootswitch_xiaozhi_available()` reports whether
  `ota_0` holds a bootable image (`esp_ota_get_partition_description()`), and
  `bootswitch_to_xiaozhi()` selects it with `esp_ota_set_boot_partition()`.
  An empty slot is refused, so a factory-fresh box cannot lose its boot.
- `POST /api/boot/xiaozhi` in `src/web/webserver.c` (behind the setup login,
  refused while the factory login is active) answers first and restarts two
  seconds later.
- `/api/config` carries `xiaozhi_available=0|1`; the settings page
  (`src/web/page.html`, `src/web/page.js`) keeps its "Start xiaozhi" button
  disabled while the slot is empty.
- `src/core/bootswitch.c` also watches the BOOT button (GPIO 0, unused by every
  mode): a two second hold switches to `ota_0` and restarts, with a short tone
  as the only feedback. `bootswitch_start_button_monitor()` starts that task
  from `app_main()`.
- `src/CMakeLists.txt` requires `app_update`, `platformio.ini` limits the
  build to the 3 MiB factory slot (currently 48.8% used).

The BOOT button was chosen because the screen has no free gesture left: a tap
switches the screen, a 0.5 s hold cycles the mode, a 5 s hold opens the setup
page.

No touch gesture is added or changed here: touch handling (`src/core/touch.c`)
stays exactly as it is — a tap acts on the screen, a 0.5 s hold cycles the
mode, a 5 s hold opens the setup page. The firmware switch lives on the BOOT
button and on the setup page.

### xiaozhi -> moonstation

Implemented in the fork (`feature/moonstation-dual-boot`), three small patches:

- `main/boards/spotpear/sp-esp32-s3-1.28-box/sp-esp32-s3-1.28-box.cc`: the
  BOOT button gets an `OnLongPress` handler, the same two second hold as in
  moonstation. It shows "Moonstation", sets the `factory` partition as boot
  partition and restarts; a short click keeps its meaning (chat / WiFi setup).
  The button is constructed with an explicit 2000 ms long press time instead of
  the component default of 1500 ms, so both firmwares agree.
- `main/application.cc`: the automatic upgrade is turned off. The server can
  offer a newer official image, and that would be written to `ota_1` — which
  then boots without the gesture, while moonstation always selects `ota_0`.
  New builds are installed over USB instead.
- `main/boards/common/system_reset.cc`: a NVS reset erases only the namespaces
  this firmware owns and keeps moonstation's (`settings`, `weather`,
  `nvs.net80211`) instead of calling `nvs_flash_erase()`. This board does not
  construct `SystemReset`, so it is protection for later, not a fix.

Fallback without a patched xiaozhi: `esptool.py erase_region 0xe000 0x2000`
over USB, next boot goes back to `factory`.

## Implementation order

Done:

1. Replaced `partitions_16mb.csv`, set `board_upload.maximum_size = 3145728`,
   updated `AGENTS.md`/`README.md` (layout, sizes, build steps).
2. Added the boot switch helper, the `app_update` dependency and the setup-page
   button to moonstation.
3. Flashed moonstation (`pio run -t upload`) and cleared the old PHY blob at
   0xe000, which is `otadata` now, with
   `esptool.py --chip esp32s3 -p /dev/ttyACM0 erase_region 0xe000 0x2000`.
   It boots from `factory`, NVS content survived, SD card, touch and audio
   still work.
4. Flashed the xiaozhi images into `ota_0` and `assets` and confirmed it boots
   there under moonstation's bootloader.
5. Patched the xiaozhi fork (BOOT button return, OTA off, NVS reset scoping),
   rebuilt it against IDF 6.1 and flashed app and assets again.
6. Tested the round trip with the BOOT button in both directions: into `ota_0`
   from moonstation, and back to `factory` with all settings intact.

Remaining:

7. Press "Start xiaozhi" on the setup page once (needs the 5 s hold on the box,
   so it cannot be done over the network).
8. Repeat the round trip after a power cycle, after a moonstation reflash and
   after a xiaozhi reflash.

## Test matrix

- factory boot after full flash, with `otadata` empty and with it set to
  `factory` — done
- switch to `ota_0`, power cycle, still `ota_0` — done (USB switch)
- 2 s BOOT hold in xiaozhi, back to `factory`, settings intact — done
- 2 s BOOT hold in moonstation, into `ota_0` — done
- reflash moonstation while `ota_0` is selected (must not change the boot
  choice, must not corrupt xiaozhi) — open
- xiaozhi asset lookup (`assets` found by label at 0xa10000) — done
- deep sleep / alarm wake in moonstation, then switch to xiaozhi — open
- WiFi has to be configured once per firmware (shared partition, separate
  credentials) — confirmed

## Effort

- partition table + moonstation switch + web button: half a day
- flashing and first end-to-end test with the existing xiaozhi binaries: 1-2 h
- xiaozhi fork patch + IDF 6.1 rebuild: half a day (mostly toolchain setup)
- optional graphical picker: 2-3 days (LVGL port to the round panel + touch)
