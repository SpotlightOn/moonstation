#ifndef DRAW_H
#define DRAW_H

#include <stdint.h>

// Drawing helpers for the shared 240x240 RGB565 frame buffer.
//
// Colors are natural RGB565 values (0xF800 = red). The byte swap the panel
// expects is done per pixel inside this module, so callers never have to
// think about it. Everything clips to the screen, so a shape may overhang
// the round panel without writing outside the buffer.

#define SCREEN_W 240
#define SCREEN_H 240

void draw_pixel(int x, int y, uint16_t color, uint16_t *fb);
void fill_rect(int x, int y, int w, int h, uint16_t color, uint16_t *fb);
void draw_line(int x0, int y0, int x1, int y1, int thickness, uint16_t color,
               uint16_t *fb);
void draw_disc(int cx, int cy, int r, uint16_t color, uint16_t *fb);
// Ring of the given thickness, drawn inward from r.
void draw_ring(int cx, int cy, int r, int thickness, uint16_t color, uint16_t *fb);

// Body text: Roboto Medium 16, proportional, grayscale antialiased. y is the
// top of the line box, so consecutive rows are draw_text_height() apart.
int draw_text_height(void);
int draw_text_width(const char *s);
void draw_text(const char *s, int x, int y, uint16_t color, uint16_t *fb);
// Same, horizontally centered, with dx as an optional offset in pixels.
void draw_text_center(const char *s, int y, int dx, uint16_t color, uint16_t *fb);
// Centered and shortened to max_w pixels, the cut marked with two dots.
void draw_text_center_fit(const char *s, int y, int max_w, uint16_t color,
                          uint16_t *fb);

// Clock digits: fixed 35x52 cells, one bit per pixel, no antialiasing.
int draw_time_height(void);
int draw_time_width(const char *s);
void draw_time_text(const char *s, int x, int y, uint16_t color, uint16_t *fb);

#endif // DRAW_H
