#ifndef TOUCH_H
#define TOUCH_H

#include "esp_err.h"
#include <stdbool.h>

typedef void (*touch_tap_cb_t)(void);
typedef void (*touch_long_cb_t)(void);

esp_err_t touch_init(touch_tap_cb_t tap_cb);
void touch_set_long_press_cb(touch_long_cb_t cb);
bool touch_is_available(void);

#endif // TOUCH_H