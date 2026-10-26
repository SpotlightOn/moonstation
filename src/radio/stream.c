#include "stream.h"
#include "audio.h"
#include "net.h"
#include "power.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mp3dec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "stream";

// Three seconds at 160 kbit/s. Big enough to ride out the bursts of a server,
// small enough that a station change does not throw away much.
#define RING_BYTES (64 * 1024)
// One network read. Small blocks keep the ring topped up: a request for the
// whole free space would block until it has all arrived.
#define READ_BLOCK 4096
// Playback only starts with this much buffered (~1.2 s at 160 kbit/s).
#define PREFILL_BYTES (24 * 1024)
// A read returns after this long even without data, so stream_stop() does not
// have to wait for the server. The connect keeps the longer default below,
// a name lookup and the handshake do not fit into a second.
#define READ_TIMEOUT_MS 1000
#define CONNECT_TIMEOUT_MS 5000
// After this long without a single byte the station counts as dead.
#define STALL_MS 10000
// PCM for one MPEG-1 frame, stereo: 1152 samples per channel.
#define PCM_SAMPLES (2 * 1152)
#define STOP_TIMEOUT_MS 7000

// The URL is copied here; only one stream runs at a time.
static char url[192];

// Ring: url[pos..used) is the compressed data waiting for the decoder. Both
// tasks take ring_lock for every change, so the decoder never reads a half
// appended frame and the network task never compacts under the decoder.
static uint8_t *ring = NULL;
static size_t pos = 0;
static size_t used = 0;
static SemaphoreHandle_t ring_lock = NULL;
static SemaphoreHandle_t ring_data = NULL;   // network task appended data
static SemaphoreHandle_t ring_space = NULL;  // decoder freed space
static SemaphoreHandle_t tasks_done = NULL;  // one give per ending task
static volatile bool running = false;
static int tasks_created = 0;

static volatile stream_state_t state = STREAM_STOPPED;
static char error_text[48];
static volatile int rate_hz = 0;
static volatile int kbps = 0;
static volatile int channels = 0;

static bool semaphores_ready(void)
{
    if (ring_lock) return true;
    ring_lock = xSemaphoreCreateMutex();
    ring_data = xSemaphoreCreateBinary();
    ring_space = xSemaphoreCreateBinary();
    tasks_done = xSemaphoreCreateCounting(2, 0);
    if (!ring_lock || !ring_data || !ring_space || !tasks_done) {
        ESP_LOGE(TAG, "no memory for the stream locks");
        return false;
    }
    return true;
}

// Ends the stream from a task. The UI sees the reason and stops showing
// "Playing"; whoever is waiting on the ring wakes up.
static void fail(const char *why)
{
    snprintf(error_text, sizeof(error_text), "%s", why);
    ESP_LOGW(TAG, "%s", why);
    running = false;
    state = STREAM_FAILED;
    xSemaphoreGive(ring_data);
    xSemaphoreGive(ring_space);
}

// Appends what was read, waiting for the decoder to make room if the ring is
// full. Dropping bytes would corrupt the MP3 stream, so it waits instead.
static void append(const uint8_t *src, size_t len)
{
    size_t done = 0;
    while (done < len && running) {
        xSemaphoreTake(ring_lock, portMAX_DELAY);
        if (RING_BYTES - used < len - done && pos > 0) {
            memmove(ring, ring + pos, used - pos);
            used -= pos;
            pos = 0;
        }
        const size_t room = RING_BYTES - used;
        size_t n = len - done;
        if (n > room) n = room;
        if (n > 0) {
            memcpy(ring + used, src + done, n);
            used += n;
            done += n;
        }
        xSemaphoreGive(ring_lock);

        if (n > 0) xSemaphoreGive(ring_data);
        if (done < len) {
            // Ring full: wait until the decoder has consumed something.
            xSemaphoreTake(ring_space, pdMS_TO_TICKS(500));
        }
    }
}

static void producer_task(void *arg)
{
    (void)arg;
    esp_http_client_handle_t http = NULL;
    uint8_t *stage = NULL;
    int64_t last_data_us = esp_timer_get_time();

    if (!net_wifi_is_up() && !net_wifi_up()) {
        fail("No WiFi");
        goto out;
    }
    net_wifi_hold();
    power_request(POWER_PROFILE_BOOST);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = CONNECT_TIMEOUT_MS,
        .buffer_size = 2048,
    };
    http = esp_http_client_init(&cfg);
    if (!http) {
        fail("Out of memory");
        goto out;
    }
    esp_http_client_set_header(http, "Icy-MetaData", "0");
    esp_http_client_set_header(http, "User-Agent", "moonstation");

    if (esp_http_client_open(http, 0) != ESP_OK) {
        fail("Connection failed");
        goto out;
    }
    esp_http_client_fetch_headers(http);
    const int status = esp_http_client_get_status_code(http);
    if (status != 200) {
        char why[24];
        snprintf(why, sizeof(why), "HTTP %d", status);
        fail(why);
        goto out;
    }
    ESP_LOGI(TAG, "connected: %s", url);
    // From here on every wait is a read wait: keep it short so a stop request
    // is honoured quickly.
    esp_http_client_set_timeout_ms(http, READ_TIMEOUT_MS);

    stage = malloc(READ_BLOCK);
    if (!stage) {
        fail("Out of memory");
        goto out;
    }
    state = STREAM_BUFFERING;
    last_data_us = esp_timer_get_time();

    while (running) {
        const int got = esp_http_client_read(http, (char *)stage, READ_BLOCK);
        if (got == -ESP_ERR_HTTP_EAGAIN) {
            // Read timeout, no data in this second. A live stream that stays
            // silent this long is dead.
            if ((esp_timer_get_time() - last_data_us) / 1000 > STALL_MS) {
                fail("No data");
                break;
            }
            continue;
        }
        if (got <= 0) {
            if (running) fail("Stream ended");
            break;
        }
        last_data_us = esp_timer_get_time();
        append(stage, (size_t)got);
    }

out:
    running = false;
    xSemaphoreGive(ring_data);
    xSemaphoreGive(ring_space);
    if (http) {
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
    }
    free(stage);
    power_release(POWER_PROFILE_BOOST);
    net_wifi_release();
    xSemaphoreGive(tasks_done);
    vTaskDelete(NULL);
}

static void decoder_task(void *arg)
{
    (void)arg;
    HMP3Decoder dec = MP3InitDecoder();
    short *pcm = malloc(PCM_SAMPLES * sizeof(short));
    bool stream_open = false;
    int64_t t0 = 0;
    // First moment the ring had too little data. A short wait is normal, only
    // a real underrun is worth a "Buffering" on screen.
    int64_t starving_since = 0;

    if (!dec || !pcm) {
        fail("Out of memory");
        goto out;
    }

    // Wait for the prefill before the first frame: a start that begins with a
    // gap sounds broken even when the stream itself is fine.
    while (running) {
        xSemaphoreTake(ring_lock, portMAX_DELAY);
        const size_t avail = used - pos;
        xSemaphoreGive(ring_lock);
        if (avail >= PREFILL_BYTES) break;
        xSemaphoreTake(ring_data, pdMS_TO_TICKS(100));
    }

    while (running) {
        if (xSemaphoreTake(ring_lock, pdMS_TO_TICKS(200)) != pdTRUE) continue;

        const size_t avail = used - pos;
        if (avail < 2048) {  // not enough for a complete frame yet
            const int64_t now = esp_timer_get_time();
            if (starving_since == 0) starving_since = now;
            // Mid-playback starvation: say so instead of leaving "Playing"
            // up while the DMA runs dry. A short wait stays invisible.
            if (state == STREAM_PLAYING && now - starving_since > 300000) {
                state = STREAM_BUFFERING;
            }
            xSemaphoreGive(ring_lock);
            xSemaphoreTake(ring_data, pdMS_TO_TICKS(avail == 0 ? 100 : 20));
            continue;
        }
        starving_since = 0;

        int off = MP3FindSyncWord(ring + pos, (int)avail);
        if (off < 0) {
            // Keep the last byte: a sync word may straddle two appends.
            pos = used - 1;
            xSemaphoreGive(ring_lock);
            xSemaphoreGive(ring_space);
            continue;
        }
        pos += off;

        const size_t before = used - pos;
        unsigned char *frame = ring + pos;
        int left = (int)before;
        const int err = MP3Decode(dec, &frame, &left, pcm, 0);
        if (err != 0) {
            // Underflow: the frame is not complete yet. Anything else: resync
            // one byte later, the next sync word will find the frame again.
            if (err != ERR_MP3_INDATA_UNDERFLOW) pos++;
            xSemaphoreGive(ring_lock);
            if (err == ERR_MP3_INDATA_UNDERFLOW) {
                xSemaphoreTake(ring_data, pdMS_TO_TICKS(20));
            }
            continue;
        }
        pos += before - (size_t)left;
        xSemaphoreGive(ring_lock);
        xSemaphoreGive(ring_space);

        MP3FrameInfo info;
        MP3GetLastFrameInfo(dec, &info);
        if (!stream_open) {
            if (audio_stream_begin(info.samprate) != ESP_OK) {
                fail("Codec failed");
                break;
            }
            stream_open = true;
            rate_hz = info.samprate;
            kbps = info.bitrate / 1000;
            channels = info.nChans;
            ESP_LOGI(TAG, "%d Hz, %d ch, %d kbit/s", info.samprate, info.nChans,
                     kbps);
        }

        // outputSamps already counts the samples of all channels, so the
        // byte count is outputSamps * sizeof(short) and not twice that.
        int samples = info.outputSamps;
        if (info.nChans == 1) {
            // The codec path is stereo; duplicate the mono frame.
            for (int i = samples - 1; i >= 0; i--) {
                pcm[i * 2] = pcm[i];
                pcm[i * 2 + 1] = pcm[i];
            }
            samples *= 2;
        }
        audio_stream_write(pcm, (size_t)samples * sizeof(short));
        if (state == STREAM_BUFFERING) {
            state = STREAM_PLAYING;
            t0 = esp_timer_get_time();
        }
    }

    if (stream_open) {
        ESP_LOGI(TAG, "playback ended after %lld ms",
                 (long long)((esp_timer_get_time() - t0) / 1000));
    }

out:
    running = false;
    audio_stream_end();
    free(pcm);
    if (dec) MP3FreeDecoder(dec);
    xSemaphoreGive(tasks_done);
    vTaskDelete(NULL);
}

esp_err_t stream_start(const char *station_url)
{
    if (!station_url || !station_url[0]) return ESP_ERR_INVALID_ARG;
    if (!semaphores_ready()) return ESP_ERR_NO_MEM;

    stream_stop();

    snprintf(url, sizeof(url), "%s", station_url);
    ring = heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ring) ring = malloc(RING_BYTES);
    if (!ring) {
        ESP_LOGE(TAG, "no memory for the ring");
        return ESP_ERR_NO_MEM;
    }
    pos = used = 0;
    rate_hz = kbps = channels = 0;
    error_text[0] = '\0';
    state = STREAM_CONNECTING;
    running = true;

    tasks_created = 0;
    if (xTaskCreate(producer_task, "radio_net", 4096, NULL, 4, NULL) == pdPASS) {
        tasks_created++;
    }
    if (xTaskCreate(decoder_task, "radio_dec", 8192, NULL, 5, NULL) == pdPASS) {
        tasks_created++;
    }
    if (tasks_created < 2) {
        running = false;
        xSemaphoreGive(ring_data);
        xSemaphoreGive(ring_space);
        stream_stop();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void stream_stop(void)
{
    if (!tasks_done) return;

    const bool was_running = running;
    running = false;
    xSemaphoreGive(ring_data);
    xSemaphoreGive(ring_space);

    // One give per task that is still alive or already finished. Both give
    // even on a failure, so the count is always right.
    bool all_stopped = true;
    for (int i = 0; i < tasks_created; i++) {
        if (xSemaphoreTake(tasks_done, pdMS_TO_TICKS(STOP_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "a stream task did not stop");
            all_stopped = false;
        }
    }
    tasks_created = 0;

    if (was_running) audio_stream_end();
    if (ring && all_stopped) {
        heap_caps_free(ring);
        ring = NULL;
    } else if (ring) {
        // A task is still in there; freeing the buffer under it would be a
        // crash. The stream is broken anyway, the next start allocates a new
        // ring and this one stays behind.
        ESP_LOGE(TAG, "ring kept, a task is still running");
    }
    pos = used = 0;
    if (state != STREAM_FAILED) state = STREAM_STOPPED;
}

stream_state_t stream_state(void)
{
    return state;
}

const char *stream_error(void)
{
    return error_text;
}

int stream_rate_hz(void)
{
    return rate_hz;
}

int stream_kbps(void)
{
    return kbps;
}

int stream_channels(void)
{
    return channels;
}
