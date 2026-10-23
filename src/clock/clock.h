#ifndef CLOCK_H
#define CLOCK_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t clock_init(void);
void clock_render(const uint16_t *background);

// True while the configurable deep sleep window (local hours) is active;
// used by the mode manager to decide when the clock mode may deep sleep.
bool clock_deep_sleep_window_active(void);

// Microseconds until the sleep window ends, 0 while the window is inactive.
// The mode manager sleeps the dark clock until then instead of waking every
// minute, so the display cannot flash at night.
int64_t clock_deep_sleep_window_end_us(void);

#endif // CLOCK_H