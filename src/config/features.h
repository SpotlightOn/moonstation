#ifndef FEATURES_H
#define FEATURES_H

// Compile time feature switches - together with settings.c the only header
// that reads config.h. A switch here decides whether code is built into the
// firmware at all; the values a feature runs with live in settings.h.
//
// Every switch needs a #ifndef fallback, so a build without config.h works.
// Keep in sync with config.h.example.

#if __has_include("config.h")
#include "config.h"
#endif

// Battery indicator below the weekday in the clock (1 = on, 0 = off). Zero
// compiles the icon, its helpers and the battery_init() call out of clock.c.
#ifndef CLOCK_SHOW_BATTERY
#define CLOCK_SHOW_BATTERY 1
#endif

// Deep sleep for the clock mode inside the night window (1 = on, 0 = off).
// Zero compiles the wake-up handling and the sleep entry out of slideshow.c;
// the window itself stays configurable at runtime either way.
#ifndef CLOCK_DEEP_SLEEP
#define CLOCK_DEEP_SLEEP 0
#endif

// Low power in the image modes (1 = on, 0 = off). One hands the CPU speed lock
// back between two images instead of holding it for the whole mode, so the
// power manager can drop the clock and let the chip light-sleep while a slide
// is on screen. Zero restores the old behaviour of a permanently acquired lock.
#ifndef SLIDE_PM_LOW_POWER
#define SLIDE_PM_LOW_POWER 1
#endif

// Automatic light sleep (1 = on, 0 = off). On, the power manager is configured
// with light sleep enabled, so the chip sleeps whenever no lock is held: in the
// image modes between two images and in the clock mode between two minutes.
// The backlight PWM and the touch interrupt pad are kept alive for this.
//
// Default off: this board talks to the host over the ESP32-S3 USB Serial/JTAG
// peripheral, which cannot survive light sleep. The port disappears while the
// chip sleeps, so flashing and monitoring break. The firmware keeps a lock
// against light sleep while a USB host is connected, but that only helps as
// long as the USB code itself is untested territory - enable this for a board
// that runs unplugged and is only flashed over a UART bridge.
#ifndef PM_LIGHT_SLEEP
#define PM_LIGHT_SLEEP 0
#endif

#endif // FEATURES_H
