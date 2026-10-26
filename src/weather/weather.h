#ifndef WEATHER_H
#define WEATHER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Weather mode: an OpenWeather screen with the current conditions, the next
// 24 hours and the next five days, one tap apart.
//
// The module owns the request, the NVS cache of the last reading and the
// rendering; the mode callbacks that drive it live in slideshow.c, the same
// split the clock uses. Everything here is compiled unconditionally so a CI
// build keeps checking it - with WEATHER_ENABLE in features.h set to zero the
// mode simply never joins the long-press cycle.

// Loads the cached reading from NVS. Call once at boot, after settings_init().
esp_err_t weather_init(void);

// Mode callbacks. enter/redraw/on_wake run in the main task, on_tap runs in
// the touch task and therefore only records the request.
void weather_enter(void);
void weather_redraw(void);
void weather_on_tap(void);
void weather_on_wake(void);

// Microseconds until the next request is due, 0 when it is due right now.
int64_t weather_next_refresh_us(void);

#endif // WEATHER_H
