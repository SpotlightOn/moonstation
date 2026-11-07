#include "player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm.h"
#include "audio.h"
#include "display.h"
#include "draw.h"
#include "esp_h264_dec.h"
#include "esp_h264_dec_param.h"
#include "esp_h264_dec_sw.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mp3dec.h"
#include "osd.h"
#include "power.h"
#include "slideshow.h" // ui_force_redraw()
#include "settings.h"
#include "ts.h"

static const char *TAG = "movie";

// One movie at a time. The engine keeps no list and no UI state; that is the
// mode in movie.c.
// Window of the video stream handed to the decoder. The software decoder
// parses access units out of a plain byte stream and reports how much it used,
// so it gets a window instead of single NAL units; 256 kB is about ten seconds
// of a 240x240 stream, and a key frame stays far below it.
#define VIDEO_BUF_BYTES (256 * 1024)
#define VIDEO_REFILL_BYTES (64 * 1024)
// PCM for one MPEG-1 frame, stereo: 1152 samples per channel.
#define PCM_SAMPLES (2 * 1152)
#define AUDIO_BUF_BYTES (16 * 1024)
#define AUDIO_REFILL_BYTES (4 * 1024)
#define STOP_TIMEOUT_MS 5000
// Frame rate of a transport stream that carries no timestamps at all. It is
// what scripts/build_movies.py writes, so the fallback is the common case.
#define MOVIE_DEFAULT_FPS 15

static const char *s_path = NULL;
static movie_info_t s_info;
static volatile bool s_running = false;
static volatile bool s_paused = false;
static volatile bool s_finished = false;
static volatile bool s_failed = false;
static int s_tasks = 0;
static bool s_power = false;
static SemaphoreHandle_t s_done = NULL;

static void finish(bool failed)
{
    s_failed = failed;
    s_finished = true;
    ESP_LOGI(TAG, "%s", failed ? "playback failed" : "playback reached the end");
    ui_force_redraw();
}

//
// Info
//

// A transport stream says nothing about the movie: no picture size, no frame
// count and no length. The size comes from the decoder with the first frame,
// the frame rate from the timestamps.
static bool read_info(const char *path, movie_info_t *out)
{
    ts_info_t info;
    ts_reader_t *r = ts_open(path, true, &info);
    if (!r) return false;

    memset(out, 0, sizeof(*out));
    out->has_audio = info.has_audio;
    out->frame_us = ts_frame_interval_us(r);
    ts_close(r);

    if (out->frame_us == 0) {
        // Without timestamps the picture would have no clock of its own.
        out->frame_us = 1000000u / MOVIE_DEFAULT_FPS;
    }
    out->fps = (uint16_t)((1000000u + out->frame_us / 2) / out->frame_us);
    return true;
}

//
// Sources: the demuxer in ts.c pulls the elementary streams out of the
// transport packets. Everything above this works on bytes, so the decoder and
// the MP3 decoder do not have to know where they came from.

static ts_reader_t *source_open(const char *path, bool video)
{
    ts_info_t info;
    return ts_open(path, video, &info);
}

static void source_close(ts_reader_t **s)
{
    if (*s) ts_close(*s);
    *s = NULL;
}

// Fills up to cap. A call that has room and returns the length unchanged has
// reached the end of the stream.
static size_t source_fill(ts_reader_t *s, uint8_t *buf, size_t len, size_t cap)
{
    return ts_fill(s, buf, len, cap);
}

// True when nothing is left to deliver. A stream that ended but still has a
// payload waiting in the reader is not at its end yet.
static bool source_dry(const ts_reader_t *s)
{
    return ts_at_end(s);
}

// Video window: slides the bytes the decoder already took down and refills the
// back. 256 kB is about ten seconds of a 240x240 stream; a key frame stays far
// below it.
static size_t window_fill(ts_reader_t *s, uint8_t *buf, size_t len, size_t off)
{
    if (off > 0) {
        memmove(buf, buf + off, len - off);
        len -= off;
    }
    return source_fill(s, buf, len, VIDEO_BUF_BYTES);
}

//
// Video
//

// I420 to the panel's RGB565 byte order. The visible area is centered in the
// 240x240 frame buffer, so a smaller movie gets a black border instead of
// landing in a corner. The target is the frame buffer shared with the other
// modes: the panel's SPI DMA cannot read from PSRAM, and a bounce buffer for a
// whole frame does not fit into internal RAM next to the decoder.
static void yuv_to_frame(const uint8_t *yuv, uint32_t out_size, int w, int h)
{
    uint16_t *frame = display_get_framebuffer();
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;
    const size_t need = (size_t)w * h + (size_t)cw * ch * 2;
    if (out_size < need) {
        ESP_LOGW(TAG, "decoded frame is %u bytes, expected %u",
                 (unsigned)out_size, (unsigned)need);
        return;
    }
    const uint8_t *yp = yuv;
    const uint8_t *up = yuv + (size_t)w * h;
    const uint8_t *vp = up + (size_t)cw * ch;

    const int vw = w < SCREEN_W ? w : SCREEN_W;
    const int vh = h < SCREEN_H ? h : SCREEN_H;
    const int ox = (SCREEN_W - vw) / 2;
    const int oy = (SCREEN_H - vh) / 2;

    for (int y = 0; y < vh; y++) {
        const uint8_t *yrow = yp + (size_t)y * w;
        const uint8_t *urow = up + (size_t)(y >> 1) * cw;
        const uint8_t *vrow = vp + (size_t)(y >> 1) * cw;
        uint16_t *drow = frame + (size_t)(oy + y) * SCREEN_W + ox;
        for (int x = 0; x < vw; x++) {
            const int c = yrow[x] - 16;
            const int d = urow[x >> 1] - 128;
            const int e = vrow[x >> 1] - 128;
            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d + 128) >> 8;
            if (r < 0) r = 0;
            if (r > 255) r = 255;
            if (g < 0) g = 0;
            if (g > 255) g = 255;
            if (b < 0) b = 0;
            if (b > 255) b = 255;
            drow[x] = RGB565_SWAP((uint16_t)(((r & 0xF8) << 8) |
                                            ((g & 0xFC) << 3) | (b >> 3)));
        }
    }
}

static void video_task(void *arg)
{
    (void)arg;
    ts_reader_t *src = NULL;
    uint8_t *buf = heap_caps_malloc(VIDEO_BUF_BYTES, MALLOC_CAP_SPIRAM |
                                                       MALLOC_CAP_8BIT);
    esp_h264_dec_handle_t dec = NULL;
    int dec_w = 0;
    int dec_h = 0;
    uint32_t pushed = 0;
    size_t len = 0; // valid bytes in buf
    size_t off = 0; // bytes of buf the decoder already took
    bool clean_end = false;
    int64_t next_us = esp_timer_get_time();
    const int64_t interval_us = s_info.frame_us;

    if (!buf || !(src = source_open(s_path, true))) {
        ESP_LOGE(TAG, "cannot open the movie stream");
        finish(true);
        goto out;
    }
    len = source_fill(src, buf, 0, VIDEO_BUF_BYTES);

    esp_h264_dec_cfg_sw_t cfg = {.pic_type = ESP_H264_RAW_FMT_I420};
    if (esp_h264_dec_sw_new(&cfg, &dec) != ESP_H264_ERR_OK || !dec) {
        ESP_LOGE(TAG, "no H.264 decoder");
        dec = NULL;
        finish(true);
        goto out;
    }
    if (esp_h264_dec_open(dec) != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "cannot open the H.264 decoder");
        finish(true);
        goto out;
    }

    while (s_running) {
        // The alarm owns the screen while it rings; the picture waits for it.
        if (s_paused || alarm_is_ringing()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            next_us = esp_timer_get_time();
            continue;
        }

        if (off == len) {
            len = source_fill(src, buf, 0, VIDEO_BUF_BYTES);
            off = 0;
            if (len == 0) {
                clean_end = true;
                break;
            }
        }

        esp_h264_dec_in_frame_t in = {0};
        esp_h264_dec_out_frame_t out = {0};
        in.raw_data.buffer = buf + off;
        in.raw_data.len = (uint32_t)(len - off);
        const esp_h264_err_t err = esp_h264_dec_process(dec, &in, &out);
        if (err != ESP_H264_ERR_OK) {
            // The stream can end in the middle of an access unit; what ran
            // before that played, so it counts as the end, not a failure.
            if (source_dry(src) && pushed > 0) {
                clean_end = true;
                break;
            }
            ESP_LOGE(TAG, "decode failed: %d", (int)err);
            finish(true);
            break;
        }
        if (in.consume == 0 || in.consume > len - off) {
            // Nothing taken: only more data can help.
            if (source_dry(src)) {
                clean_end = true; // trailing bytes without a picture
                break;
            }
            if (off == 0 && len == VIDEO_BUF_BYTES) {
                ESP_LOGE(TAG, "no access unit in %d bytes", VIDEO_BUF_BYTES);
                finish(true);
                break;
            }
            len = window_fill(src, buf, len, off);
            off = 0;
            continue;
        }
        off += in.consume;

        if (len - off < VIDEO_REFILL_BYTES) {
            len = window_fill(src, buf, len, off);
            off = 0;
        }

        if (out.out_size == 0) continue; // SPS, PPS, SEI: no picture yet

        if (dec_w == 0) {
            esp_h264_dec_param_sw_handle_t param = NULL;
            esp_h264_resolution_t res = {0};
            if (esp_h264_dec_sw_get_param_hd(dec, &param) == ESP_H264_ERR_OK &&
                esp_h264_dec_get_resolution(param, &res) == ESP_H264_ERR_OK) {
                dec_w = res.width;
                dec_h = res.height;
            }
            if (dec_w <= 0 || dec_h <= 0) {
                ESP_LOGE(TAG, "the stream carries no readable picture size");
                finish(true);
                break;
            }
            ESP_LOGI(TAG, "stream %dx%d, %u us per frame", dec_w, dec_h,
                     (unsigned)s_info.frame_us);
        }

        yuv_to_frame(out.outbuf, out.out_size, dec_w, dec_h);
        // A volume bar that is up belongs on this frame too; the mode manager
        // only pushes it on its own when no picture is running.
        osd_paint(display_get_framebuffer());
        display_draw_bitmap(0, 0, SCREEN_W, SCREEN_H,
                            display_get_framebuffer());
        pushed++;

        next_us += interval_us;
        const int64_t now = esp_timer_get_time();
        if (next_us > now) {
            vTaskDelay(pdMS_TO_TICKS((next_us - now) / 1000));
        } else if (now - next_us > 3 * interval_us) {
            // Far behind (a long pause, a slow decode): restart the clock
            // instead of racing through the backlog.
            next_us = now;
        }
    }

    if (clean_end && s_running) {
        // An empty stream is a broken file, not a movie that is over.
        finish(pushed == 0);
    }

out:
    s_running = false; // the audio task has no reason to outlive the picture
    if (dec) {
        esp_h264_dec_close(dec);
        esp_h264_dec_del(dec);
    }
    source_close(&src);
    heap_caps_free(buf);
    if (s_done) xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

//
// Audio
//

static void audio_task(void *arg)
{
    (void)arg;
    ts_reader_t *src = NULL;
    HMP3Decoder dec = NULL;
    int16_t *pcm = NULL;
    uint8_t *buf = NULL;
    bool codec_open = false;
    size_t len = 0;

    if (!s_info.has_audio) goto out;

    dec = MP3InitDecoder();
    pcm = malloc(PCM_SAMPLES * sizeof(int16_t));
    buf = heap_caps_malloc(AUDIO_BUF_BYTES, MALLOC_CAP_SPIRAM |
                                              MALLOC_CAP_8BIT);
    if (!dec || !pcm || !buf || !(src = source_open(s_path, false))) {
        ESP_LOGW(TAG, "no audio for this movie");
        goto out;
    }

    while (s_running) {
        // Pause and alarm keep the decoder state; the codec closes so the
        // speaker stays quiet until playback continues.
        if (s_paused || alarm_is_ringing()) {
            if (codec_open) {
                audio_stream_end();
                codec_open = false;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (len < AUDIO_REFILL_BYTES) len = source_fill(src, buf, len, AUDIO_BUF_BYTES);
        if (len == 0) break;

        const int off = MP3FindSyncWord(buf, (int)len);
        if (off < 0) {
            len = 0; // no frame in what is left, refill on the next pass
            continue;
        }
        if (off > 0) {
            memmove(buf, buf + off, len - (size_t)off);
            len -= (size_t)off;
        }

        int bytes = (int)len;
        unsigned char *frame = buf;
        const int err = MP3Decode(dec, &frame, &bytes, pcm, 0);
        const size_t consumed = len - (size_t)(bytes < 0 ? 0 : bytes);

        if (err != 0) {
            if (err == ERR_MP3_INDATA_UNDERFLOW) {
                const size_t before = len;
                len = source_fill(src, buf, len, AUDIO_BUF_BYTES);
                if (len == before) {
                    if (len == AUDIO_BUF_BYTES) {
                        // A broken frame that never completes: drop a byte
                        // and let the sync search find the next one.
                        memmove(buf, buf + 1, len - 1);
                        len--;
                    } else {
                        break; // stream ended on a partial frame
                    }
                }
            } else if (consumed > 0) {
                memmove(buf, buf + consumed, len - consumed);
                len -= consumed;
            } else {
                memmove(buf, buf + 1, len - 1);
                len--;
            }
            continue;
        }

        if (consumed > 0) {
            memmove(buf, buf + consumed, len - consumed);
            len -= consumed;
        }

        MP3FrameInfo info;
        MP3GetLastFrameInfo(dec, &info);
        if (!codec_open) {
            if (audio_stream_begin(info.samprate) != ESP_OK) {
                ESP_LOGW(TAG, "codec refused %d Hz", info.samprate);
                break;
            }
            codec_open = true;
            ESP_LOGI(TAG, "audio %d Hz, %d ch, %d kbit/s", info.samprate,
                     info.nChans, info.bitrate / 1000);
        }

        int samples = info.outputSamps;
        if (info.nChans == 1) {
            for (int i = samples - 1; i >= 0; i--) {
                pcm[i * 2] = pcm[i];
                pcm[i * 2 + 1] = pcm[i];
            }
            samples *= 2;
        }
        if (audio_stream_write(pcm, (size_t)samples * sizeof(int16_t)) !=
            ESP_OK) {
            break;
        }
    }

out:
    if (codec_open) audio_stream_end();
    source_close(&src);
    if (dec) MP3FreeDecoder(dec);
    free(pcm);
    heap_caps_free(buf);
    if (s_done) xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

//
// Lifecycle
//

static void release(void)
{
    for (int i = 0; i < s_tasks; i++) {
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(STOP_TIMEOUT_MS)) != pdTRUE) {
            // The frame buffer is shared, so a task that is late is not a
            // reason to crash; the next start does not touch it before the
            // stop above has run again.
            ESP_LOGW(TAG, "a playback task did not stop");
        }
    }
    s_tasks = 0;
    if (s_power) {
        power_release(POWER_PROFILE_BOOST);
        s_power = false;
    }
    s_running = false;
    s_paused = false;
    s_finished = false;
    s_failed = false;
    s_path = NULL;
    memset(&s_info, 0, sizeof(s_info));
}

// A file the player cannot identify is worth a line that says what it actually
// is: the first bytes separate an MP4 ("ftyp" behind the box size), a Matroska
// (1A 45 DF A3), a WebM/RIFF and a broken transport stream from each other.
static void log_unplayable(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return;
    }
    uint8_t head[8] = {0};
    const size_t got = fread(head, 1, sizeof(head), f);
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fclose(f);

    char hex[sizeof(head) * 3 + 1] = {0};
    for (size_t i = 0; i < got; i++) {
        snprintf(hex + i * 3, 4, "%02x ", head[i]);
    }
    ESP_LOGE(TAG, "%s: %ld bytes, starts with %s", path, size, hex);
}

esp_err_t movie_player_start(const char *path)
{
    if (!path || !path[0]) return ESP_ERR_INVALID_ARG;
    movie_player_stop();

    movie_info_t info;
    const bool ok = read_info(path, &info);
    if (!ok) {
        log_unplayable(path);
        ESP_LOGE(TAG, "%s is not a movie this firmware can play", path);
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_done) {
        s_done = xSemaphoreCreateCounting(2, 0);
        if (!s_done) return ESP_ERR_NO_MEM;
    }
    // A stop that timed out can leave a count behind; it must not stand in for
    // the tasks this start creates.
    while (xSemaphoreTake(s_done, 0) == pdTRUE) {
    }

    s_path = path;
    s_info = info;
    s_paused = false;
    s_finished = false;
    s_failed = false;
    s_running = true;

    // Decoding H.264 in software next to the panel traffic needs the top
    // clock step, the same reason the radio asks for it.
    power_request(POWER_PROFILE_BOOST);
    s_power = true;

    s_tasks = 0;
    if (xTaskCreate(video_task, "movie_vid", 10240, NULL, 5, NULL) == pdPASS) {
        s_tasks++;
    }
    if (s_tasks == 1 &&
        xTaskCreate(audio_task, "movie_aud", 6144, NULL, 4, NULL) == pdPASS) {
        s_tasks++;
    }
    if (s_tasks < 2) {
        s_running = false;
        release();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "playing %s: MPEG-TS, H.264%s, %u us per frame", path,
             info.has_audio ? " + MP3" : "", (unsigned)info.frame_us);
    return ESP_OK;
}

void movie_player_stop(void)
{
    if (!s_done) return;
    if (!s_running && s_tasks == 0 && !s_path) return;
    s_running = false;
    release();
}

bool movie_player_is_playing(void)
{
    return s_running;
}

void movie_player_set_paused(bool paused)
{
    s_paused = paused;
}

bool movie_player_is_paused(void)
{
    return s_paused;
}

bool movie_player_finished(void)
{
    return s_finished;
}

bool movie_player_failed(void)
{
    return s_failed;
}

const movie_info_t *movie_player_info(void)
{
    return s_path ? &s_info : NULL;
}

uint16_t *movie_player_framebuffer(void)
{
    // The shared frame buffer holds the picture that is on the panel, so the
    // pause overlay can dim it in place. NULL keeps the caller honest about
    // drawing only while a movie runs.
    return s_path ? display_get_framebuffer() : NULL;
}
