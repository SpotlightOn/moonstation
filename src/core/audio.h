#ifndef AUDIO_H
#define AUDIO_H

#include "esp_err.h"

esp_err_t audio_init(void);

// Plays a sine tone at the configured volume (AUDIO_VOLUME).
void audio_play_tone(int freq_hz, int duration_ms);

// Same, but with an explicit output volume in percent (0-100). Used for
// tones that should stay in the background, like the low battery beep.
void audio_play_tone_at(int freq_hz, int duration_ms, int volume_pct);

// Sets the output volume in percent (0-100). Unlike the tone functions this
// does not open or close the codec: it takes effect immediately, including on
// a stream that is already open.
void audio_set_volume(int percent);

#endif // AUDIO_H
