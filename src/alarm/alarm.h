#ifndef ALARM_H
#define ALARM_H

#include <stdbool.h>
#include <stdint.h>

// Daily alarm for one local time. The module owns the ringing state, the beep
// pattern and the screen it shows while it rings; the mode manager calls
// alarm_on_wake() before every mode redraw, so the alarm wins over the mode,
// and alarm_next_wake_us() keeps the loop (and the night deep sleep) from
// missing the alarm time.
//
// The alarm has no settings of its own: alarm_enabled, alarm_hour and
// alarm_minute are ordinary entries of settings_t and therefore editable on
// the setup page.

// True while the alarm is ringing, so the caller can keep the screen lit, skip
// the mode's redraw and let a tap stop it instead of acting on the mode.
bool alarm_is_ringing(void);

// One pass of the main loop: starts the alarm when it is due, plays the next
// beep of the pattern and paints the ringing screen. Does nothing while the
// alarm is off or when it already rang today.
void alarm_on_wake(void);

// Stops a ringing alarm. Safe to call from the touch task.
void alarm_dismiss(void);

// Microseconds until the next thing to do: the alarm time itself while it is
// waiting, one tick while it rings, INT64_MAX while it is off. Callers use it
// to shorten their own wake-up.
int64_t alarm_next_wake_us(int64_t now_us);

#endif // ALARM_H
