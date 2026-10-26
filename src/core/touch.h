#ifndef TOUCH_H
#define TOUCH_H

#include "esp_err.h"
#include <stdbool.h>

typedef void (*touch_tap_cb_t)(int x, int y);
typedef void (*touch_long_cb_t)(void);
typedef void (*touch_setup_cb_t)(void);
typedef void (*touch_swipe_cb_t)(int dx, int dy, int duration_ms);

// Fires on release of a touch that did not move (see TOUCH_SWIPE_MIN_PX) and
// lasted less than TOUCH_TAP_MAX_MS. x/y is where the finger landed, in panel
// pixels, so a mode can turn parts of the screen into buttons.
esp_err_t touch_init(touch_tap_cb_t tap_cb);
void touch_set_long_press_cb(touch_long_cb_t cb);

// Fires once when a touch that moved at least TOUCH_SWIPE_MIN_PX is released.
// dx/dy are the displacement of the farthest point from where the finger
// landed, in pixels; the axes are not normalized, the receiver picks the
// dominant one. A swipe never also fires a tap or a long press.
void touch_set_swipe_cb(touch_swipe_cb_t cb);

// Fires when the finger stays at one spot for TOUCH_SETUP_HOLD_MS: the
// gesture that opens the setup web server.
void touch_set_setup_cb(touch_setup_cb_t cb);
bool touch_is_available(void);

#endif // TOUCH_H