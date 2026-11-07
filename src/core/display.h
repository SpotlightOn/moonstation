#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

esp_err_t display_init(void);
esp_lcd_panel_handle_t display_get_panel(void);

// The panel has one drawer at a time. Its SPI driver is not written for two
// tasks pushing at once - the movie mode's picture task and the mode manager's
// overlay share it - and a mixed polling/queued sequence from two tasks can
// leave a transfer that never completes. Every push goes through
// display_draw_bitmap(); display_lock()/display_unlock() wrap a group of pushes
// that must not be interrupted by another task's frame.
void display_lock(void);
void display_unlock(void);
void display_draw_bitmap(int x0, int y0, int x1, int y1, const uint16_t *px);

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

// Configure the backlight LEDC channel but leave the pad dark. Used on deep
// sleep wake-ups, where the mode manager decides whether the display lights
// up at all; later brightness changes work either way.
void display_backlight_init(void);

// Put the panel and the backlight to sleep and latch the SPI pads with GPIO
// holds before the firmware enters deep sleep. display_init() releases the
// holds again after the next wake-up and reinitializes the panel.
void display_deep_sleep_prepare(void);

#endif // DISPLAY_H
