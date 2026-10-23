#ifndef SLIDESHOW_H
#define SLIDESHOW_H

void slideshow_run(void);

// Asks the mode manager for an immediate redraw: the next loop pass renders
// even if the minute has not changed. Safe to call from any task.
void ui_force_redraw(void);

#endif // SLIDESHOW_H