#ifndef TS_H
#define TS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// MPEG-TS reader of the movie mode: pulls one elementary stream out of a
// transport stream, without the PES headers, so the decoder sees the same
// Annex-B or MP3 byte stream a raw file would give it.
//
// The firmware only has to understand what ffmpeg writes for a movie: one
// program, one H.264 stream in Annex-B and at most one MPEG audio stream. That
// keeps the container standard (VLC, mpv and ffmpeg open the file on the card
// as it is) without a full demuxer: no sample tables, no EBML and no AVCC to
// Annex-B conversion.
//
// Two readers may run on the same file at once (one for the picture, one for
// the sound); each opens its own handle and skips the packets of the other
// stream.

typedef struct ts_reader ts_reader_t;

typedef struct {
    bool has_video;
    bool has_audio;
    uint8_t audio_type; // stream type of the audio stream, for the log
} ts_info_t;

// Opens path and locates the wanted stream. info receives what the program
// contains, also when the wanted stream is missing. Returns NULL when the file
// is not a readable transport stream or the stream is missing.
ts_reader_t *ts_open(const char *path, bool want_video, ts_info_t *info);
void ts_close(ts_reader_t *r);

// Appends up to cap - len bytes of the stream payload to buf and returns the
// new length. A call that has room and returns the length unchanged has
// reached the end of the stream.
size_t ts_fill(ts_reader_t *r, uint8_t *buf, size_t len, size_t cap);

// True once a fill found nothing left to deliver.
bool ts_at_end(const ts_reader_t *r);

// Presentation timestamp of the last PES header read, in 90 kHz ticks. The
// picture task uses the difference between two of them to pace the frames.
uint32_t ts_last_pts(ts_reader_t *r);

// Frame interval in microseconds, averaged over the timestamps of the first
// frames. A transport stream has no header with a frame rate, but ffmpeg
// writes one PES per access unit, so the delta between two PES headers is the
// duration of one frame. Returns 0 when the stream carries no usable
// timestamps; the file is rewound either way.
uint32_t ts_frame_interval_us(ts_reader_t *r);

#endif // TS_H
