#include "status.h"
#include "display.h"

struct ui_status ui_status = {0};

// Red dot at the bottom: the only sign on a 240 px screen that the setup
// server is reachable, without spending a text row on it. The round panel
// cuts off the corners, so the dot sits in the middle of the lower edge and
// a little above it, where it is still whole.
void ui_status_draw(uint16_t *fb)
{
    if (!ui_status.web_server) return;

    const int cx = 120;
    const int cy = 210;
    const int r = 6;
    const uint16_t px = RGB565_SWAP(0xF800); // plain RGB565 red
    for (int y = cy - r; y <= cy + r; y++) {
        for (int x = cx - r; x <= cx + r; x++) {
            int dx = x - cx;
            int dy = y - cy;
            if (dx * dx + dy * dy > r * r) continue;
            if (x < 0 || x >= 240 || y < 0 || y >= 240) continue;
            fb[y * 240 + x] = px;
        }
    }
}
