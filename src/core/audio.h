#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>
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

// Streaming output for a decoder. The codec stays open between the calls, so
// a stream does not pay a re-open per buffer and the sample rate can differ
// from the tone rate. PCM is interleaved 16 bit stereo; the caller owns the
// clock (the write blocks until the I2S DMA has room, which paces the decode
// loop). Tones still work while a stream is open: they are written into the
// same codec at its rate instead of re-opening it.
esp_err_t audio_stream_begin(int sample_rate_hz);
esp_err_t audio_stream_write(const void *pcm, size_t bytes);
void audio_stream_end(void);

#endif // AUDIO_H
