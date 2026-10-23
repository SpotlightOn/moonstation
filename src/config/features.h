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

#endif // FEATURES_H
