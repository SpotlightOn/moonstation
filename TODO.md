# TODO

Not in the firmware yet, in no particular order.

## PSRAM

The box has 8 MB octal PSRAM at 80 MHz (measured, see the Hardware section of
the README), but `CONFIG_SPIRAM` is off, so the firmware runs on the internal
RAM. Enabling it would turn ~336 kB of heap into ~8.7 MB - room for larger JPEG
buffers, bigger responses and whatever the next features need. Nothing depends
on it today, so this is a comfortable change rather than a need.

## Xiaozhi

Spotpear sells this board as an AI desktop assistant running Xiaozhi, and the
project ships a board definition for it (`Spotpear ESP32-S3-1.28-BOX`). The
hardware fits: 16 MB flash, 8 MB PSRAM, ES8311 audio, CST816D touch, GC9A01
panel. Note that Xiaozhi replaces this firmware - it brings its own partition
table and application - so it is an alternative, not an addition.
