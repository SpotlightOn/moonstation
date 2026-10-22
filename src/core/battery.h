#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>
#include "esp_err.h"

// Prepares the battery ADC (PIN_BAT_ADC) and the charging input
// (PIN_BAT_CHG). Returns ESP_OK when readings are available.
esp_err_t battery_init(void);

// Charge level in percent (0-100), averaged over a few ADC samples.
// Returns -1 when the battery monitor is not available.
int battery_level(void);

// True while the board reports that the battery is being charged.
bool battery_is_charging(void);

#endif // BATTERY_H
