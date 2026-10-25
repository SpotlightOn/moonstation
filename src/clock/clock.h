#ifndef CLOCK_H
#define CLOCK_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t clock_init(void);

// Points localtime() at the configured zone from settings. Called at boot
// and again when the setup page changes the timezone.
void clock_apply_timezone(void);
void clock_render(const uint16_t *background);

// Draws the clock overlay into an existing 240x240 panel-order frame without
// pushing it to the panel. Combined mode composes a slide this way before a
// crossfade, so the clock is identical in both frames and stays static.
void clock_compose(uint16_t *fb);

// True while the configurable deep sleep window (local hours) is active;
// used by the mode manager to decide when the clock mode may deep sleep.
bool clock_deep_sleep_window_active(void);

// Microseconds until the sleep window ends, 0 while the window is inactive.
// The mode manager sleeps the dark clock until then instead of waking every
// minute, so the display cannot flash at night.
int64_t clock_deep_sleep_window_end_us(void);

#endif // CLOCK_H