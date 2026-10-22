#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

esp_err_t display_init(void);
esp_lcd_panel_handle_t display_get_panel(void);

// Byte swap for RGB565 pixel values written into a frame buffer. The panel
// expects the most significant byte first on the bus; esp_lcd only reverses
// command and parameter bytes, not color data, and the CPU stores a
// uint16_t least significant byte first. Palindromic values such as white
// and black are unaffected by the swap.
#define RGB565_SWAP(c) ((uint16_t)(((uint16_t)(c) << 8) | ((uint16_t)(c) >> 8)))

// Single 240x240 RGB565 frame buffer shared by the slideshow and the clock.
// DRAM is scarce on this part, so the firmware only pays for one of them.
uint16_t *display_get_framebuffer(void);

void display_backlight_on(void);
void display_backlight_off(void);
void display_backlight_set_brightness(int percent);

#endif // DISPLAY_H