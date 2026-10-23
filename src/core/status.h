#ifndef STATUS_H
#define STATUS_H

#include <stdbool.h>
#include <stdint.h>

// Runtime flags the UI has to show without asking the module that owns them.
// The web server writes them, clock and slideshow only read them, so neither
// side needs to include the other.
struct ui_status {
    bool web_server; // setup web server is listening
    char ip[16];     // its address, empty while it does not listen
};

extern struct ui_status ui_status;

// Draws the status markers into a 240x240 RGB565 frame buffer. Call after
// the content of the buffer is final and before it goes to the panel.
void ui_status_draw(uint16_t *fb);

#endif // STATUS_H
