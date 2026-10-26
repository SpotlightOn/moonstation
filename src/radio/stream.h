#ifndef STREAM_H
#define STREAM_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// MP3 over HTTP for the radio mode.
//
// Two tasks share a 64 kB ring buffer in PSRAM: a network task that reads the
// stream in small blocks and may block for as long as the server needs, and a
// decoder task that takes frames out of the ring and writes PCM to the codec.
// They have to be separate: esp_http_client_read() only returns once it has
// read the full requested length, so a read inside the decode loop starves the
// I2S DMA and is heard as a gap.
//
// One stream at a time. stream_start() replaces a running one, stream_stop()
// blocks until both tasks are gone.

typedef enum {
    STREAM_STOPPED = 0,  // nothing is running
    STREAM_CONNECTING,   // the HTTP request is in flight
    STREAM_BUFFERING,    // connected, waiting for the prefill
    STREAM_PLAYING,      // frames reach the codec
    STREAM_FAILED,       // the last attempt ended, stream_error() says why
} stream_state_t;

// Starts url in the background. Returns ESP_OK when the tasks are up; what
// happens with the connection is reported through the state.
esp_err_t stream_start(const char *url);

// Stops the tasks and closes the codec. Safe to call when nothing runs.
void stream_stop(void);

stream_state_t stream_state(void);

// Short description of the last failure, empty when there was none. Only
// meaningful while the state is STREAM_FAILED.
const char *stream_error(void);

// Format of the stream, from the first decoded frame. Zero until then.
int stream_rate_hz(void);
int stream_kbps(void);
int stream_channels(void);

#endif // STREAM_H
