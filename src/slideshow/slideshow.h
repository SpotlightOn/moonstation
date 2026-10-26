#ifndef SLIDESHOW_H
#define SLIDESHOW_H

void slideshow_run(void);

// Asks the mode manager for an immediate redraw: the next loop pass renders
// even if the minute has not changed. Safe to call from any task.
void ui_force_redraw(void);

// Changes the volume by the given number of 5 % steps on behalf of a mode that
// draws its own volume buttons (the radio). The main loop applies it on the
// next pass, with the bar, exactly like a swipe. Call from the main task.
void slideshow_volume_steps(int steps);

#endif // SLIDESHOW_H
