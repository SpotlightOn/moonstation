#ifndef SDCARD_H
#define SDCARD_H

#include "esp_err.h"
#include <stdbool.h>

esp_err_t sdcard_init(void);
esp_err_t sdcard_deinit(void);
bool sdcard_is_mounted(void);

#endif // SDCARD_H