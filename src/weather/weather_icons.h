#ifndef WEATHER_ICONS_H
#define WEATHER_ICONS_H

#include <stdbool.h>
#include <stdint.h>

// Small condition icons for the weather mode, drawn with the primitives of
// draw.h so they need no image data and scale with the requested size.

typedef enum {
    WEATHER_ICON_CLEAR,
    WEATHER_ICON_PARTLY,
    WEATHER_ICON_CLOUD,
    WEATHER_ICON_FOG,
    WEATHER_ICON_DRIZZLE,
    WEATHER_ICON_RAIN,
    WEATHER_ICON_SNOW,
    WEATHER_ICON_STORM,
} weather_icon_kind_t;

// Maps an OpenWeather condition id (the "id" of weather[0]) onto a drawing.
weather_icon_kind_t weather_icon_kind(int cond);

// Draws the icon centered on (cx, cy). size is the width of its box in pixels,
// the height comes out slightly smaller. night switches the sun for a moon.
void weather_icon_draw(int cx, int cy, int size, weather_icon_kind_t kind,
                       bool night, uint16_t *fb);

#endif // WEATHER_ICONS_H
