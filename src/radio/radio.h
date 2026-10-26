#ifndef RADIO_H
#define RADIO_H

#include <stdint.h>

// Internet radio mode: plays the MP3 streams of the .pls files in
// settings.radio_dir on the SD card.
//
// The mode only owns the station list, the screen and the gestures; the
// playback itself is src/radio/stream.c. A tap plays and pauses, a horizontal
// swipe changes the station, the long press leaves the mode. Everything here
// is compiled unconditionally so a CI build keeps checking it - with
// RADIO_ENABLE in features.h set to zero the mode never joins the cycle.

// Mode callbacks. enter/exit/on_wake/next_wake_us run in the main task;
// on_tap and on_swipe run in the touch task and only record the request.
//
// A tap on the button in the middle of the screen plays or pauses; a tap above
// or below it changes the volume, so an accidental tap never stops the station.
// A swipe left or right changes the station.
void radio_enter(void);
void radio_exit(void);
// Draws the current state again (e.g. after the status row changed).
void radio_redraw(void);
void radio_on_tap(int x, int y);
void radio_on_swipe(int dx, int dy);
void radio_on_wake(void);

// Microseconds until the mode wants to be woken again.
int64_t radio_next_wake_us(int64_t now_us);

#endif // RADIO_H
