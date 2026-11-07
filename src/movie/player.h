#ifndef PLAYER_H
#define PLAYER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Playback engine of the movie mode: decodes one movie file and keeps the
// picture and the sound running on the panel and on the speaker.
//
// The container is MPEG-TS, what scripts/build_movies.py writes: one H.264
// Annex-B stream and an optional MP3 stream in transport packets; the demuxer
// in ts.c takes the PES headers off. The file plays as it is in VLC, mpv and
// ffmpeg, which is the point of the container: nothing has to be converted to
// be looked at on a desktop.
//
// The video is decoded by the esp_h264 software decoder into I420 and
// converted to RGB565 for the panel. The audio is decoded by the MP3 decoder
// of the radio mode and written through the codec, so the volume setting, the
// tones and the alarm keep working unchanged.
//
// The engine runs two tasks: one paces the pictures (decode, convert, push,
// wait for the next frame slot) and one feeds the codec, which paces itself on
// the I2S DMA. Both stop while the mode is paused and while the alarm rings,
// so the alarm screen is not painted over by the next frame.

typedef struct {
    // A transport stream knows none of this up front: width and height come
    // from the decoder with the first frame.
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint32_t frame_us; // duration of one frame, the playback clock
    uint32_t frame_count;
    bool has_audio;
} movie_info_t;

// Starts path from its first frame. Stops a movie that is still playing.
// Returns ESP_ERR_INVALID_ARG for a file that is not a readable transport
// stream, ESP_ERR_NOT_FOUND when it cannot be opened and ESP_ERR_NO_MEM when
// the tasks cannot be created.
esp_err_t movie_player_start(const char *path);

// Stops both tasks and releases the frame buffer. Safe to call when nothing
// plays.
void movie_player_stop(void);

// True between start and stop, even when paused.
bool movie_player_is_playing(void);

// Freezes the picture and the sound where they are; false continues both.
void movie_player_set_paused(bool paused);
bool movie_player_is_paused(void);

// True once the video stream reached its end or failed. The mode then either
// loops the movie or shows the error; the engine itself does not restart.
bool movie_player_finished(void);
bool movie_player_failed(void);

// Header of the running movie, NULL when nothing plays.
const movie_info_t *movie_player_info(void);

// Panel-order RGB565 buffer holding the picture that is on the panel, NULL
// when nothing plays. It is the frame buffer shared with the other modes
// (the panel's SPI DMA cannot read PSRAM); the mode draws its pause overlay
// into it and pushes it, so the overlay lands on the frame that is on screen.
uint16_t *movie_player_framebuffer(void);

#endif // PLAYER_H
