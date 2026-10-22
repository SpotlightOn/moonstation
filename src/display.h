#ifndef DISPLAY_H
#define DISPLAY_H

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

esp_err_t display_init(void);
esp_lcd_panel_handle_t display_get_panel(void);
void display_backlight_on(void);
void display_backlight_off(void);
void display_backlight_set_brightness(int percent);

#endif // DISPLAY_H