#ifndef MOVIE_H
#define MOVIE_H

#include <stdint.h>

// Movie mode: plays the MPEG-TS (.ts) files in settings.movie_dir on the SD
// card, one file after the other in name order. A swipe left or right picks the
// next or previous movie, a tap pauses and continues, the long press leaves the
// mode. The volume keeps the global vertical swipe and its overlay.
//
// The mode owns the list, the screen and the gestures; the decoding lives in
// src/movie/player.c. Everything here is compiled unconditionally so a CI
// build keeps checking it - with MOVIE_ENABLE in features.h set to zero the
// mode never joins the cycle.
//
// The pictures are full screen while they play, so there is nothing to draw
// between two frames. Only a pause or an error puts text on the panel.

// Mode callbacks. enter/exit/on_wake/next_wake_us run in the main task;
// on_tap and on_swipe run in the touch task and only record the request.
void movie_enter(void);
void movie_exit(void);
// Draws the current state again (e.g. after a pause or a status change).
void movie_redraw(void);
void movie_on_tap(int x, int y);
void movie_on_swipe(int dx, int dy);
void movie_on_wake(void);

// Microseconds until the mode wants to be woken again.
int64_t movie_next_wake_us(int64_t now_us);

#endif // MOVIE_H
