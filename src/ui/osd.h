#ifndef OSD_H
#define OSD_H

#include <stdint.h>

// Short overlay on top of whatever the active mode shows. It is drawn into the
// shared frame buffer and pushed to the panel as one rectangle, so it works in
// every mode without the mode knowing about it. After a little over a second
// the covered area is restored from a PSRAM copy - but only if no mode painted
// over the overlay in the meantime, otherwise the old pixels would erase newer
// work.
//
// Everything runs in the main task (the mode manager). The touch task only
// leaves its wish behind and wakes the loop, the same rule the modes follow.

void osd_init(void);

// Shows the volume bar for one and a half seconds, replacing a bar that is
// still up.
void osd_show_volume(int percent);

// Restores the covered area once its time is up. Call from the main loop.
void osd_on_wake(int64_t now_us);

// Time until the overlay has to be taken down, or INT64_MAX while idle.
// Feeds the mode manager's wake-up calculation.
int64_t osd_next_wake_us(int64_t now_us);

#endif // OSD_H
