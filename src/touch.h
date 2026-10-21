#ifndef TOUCH_H
#define TOUCH_H

#include "esp_err.h"
#include <stdbool.h>

typedef void (*touch_tap_cb_t)(void);

esp_err_t touch_init(touch_tap_cb_t tap_cb);
bool touch_is_available(void);

#endif // TOUCH_H