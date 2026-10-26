#include "audio.h"
#include "pins.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_err.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "math.h"
#include <stdlib.h>

static const char *TAG = "audio";
static esp_codec_dev_handle_t codec_dev = NULL;
static i2s_chan_handle_t tx_handle = NULL;
static esp_codec_dev_sample_info_t fs = {
    .sample_rate = 24000,
    .channel = 2,
    .bits_per_sample = 16,
};

// One mutex for everything that touches the codec: the tone calls come from
// the touch task and the mode manager, a stream comes from the decoder task.
// Without it an alarm beep could land in the middle of a stream buffer.
#define AUDIO_LOCK_MS 2000
// Tone samples per write. The tone has to fit on the stack of whoever plays
// it (the touch task has 3 kB), so it is generated in blocks on the heap.
#define TONE_CHUNK 512

static SemaphoreHandle_t audio_lock = NULL;
static bool stream_open = false;  // codec is open for a decoder
static int stream_rate = 0;       // its sample rate, for tones in between

static bool audio_lock_take(void)
{
    return audio_lock &&
           xSemaphoreTake(audio_lock, pdMS_TO_TICKS(AUDIO_LOCK_MS)) == pdTRUE;
}

static void audio_lock_give(void)
{
    if (audio_lock) xSemaphoreGive(audio_lock);
}

// Callers already hold the lock.
static void set_volume_locked(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    esp_codec_dev_set_out_vol(codec_dev, percent);
}

static i2c_master_bus_handle_t init_i2c(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_AUD_I2C_SDA,
        .scl_io_num = PIN_AUD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t ret = i2c_new_master_bus(&cfg, &bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed: %s", esp_err_to_name(ret));
    }
    return bus;
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (ret != ESP_OK) return ret;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_AUD_MCLK,
            .bclk = PIN_AUD_BCLK,
            .ws = PIN_AUD_WS,
            .dout = PIN_AUD_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK) return ret;
    ret = i2s_channel_enable(tx_handle);
    return ret;
}

esp_err_t audio_init(void)
{
    audio_lock = xSemaphoreCreateMutex();
    if (!audio_lock) {
        ESP_LOGE(TAG, "codec lock failed");
        return ESP_ERR_NO_MEM;
    }

    // After a deep sleep wake-up the PA enable pad is still latched low
    // (off) by the hold taken for deep sleep; release it before the codec
    // driver reconfigures the pin. Never held on normal boots, so this is
    // always safe to call.
    gpio_hold_dis((gpio_num_t)PIN_AUD_PA);

    i2c_master_bus_handle_t i2c_bus = init_i2c();
    if (!i2c_bus) return ESP_FAIL;

    esp_err_t ret = init_i2s();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) {
        ESP_LOGE(TAG, "ctrl interface failed");
        return ESP_FAIL;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .tx_handle = tx_handle,
        .rx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) {
        ESP_LOGE(TAG, "data interface failed");
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PIN_AUD_PA,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .mclk_div = 256,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) {
        ESP_LOGE(TAG, "codec interface failed");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    codec_dev = esp_codec_dev_new(&dev_cfg);
    if (!codec_dev) {
        ESP_LOGE(TAG, "codec dev failed");
        return ESP_FAIL;
    }

    // Test tone (opens, plays and closes the codec, keeping PA off afterwards).
    audio_play_tone(440, 200);
    ESP_LOGI(TAG, "audio ready");
    return ESP_OK;
}

void audio_play_tone(int freq_hz, int duration_ms)
{
    audio_play_tone_at(freq_hz, duration_ms, settings.audio_volume);
}

void audio_set_volume(int percent)
{
    if (!codec_dev) return;
    if (!audio_lock_take()) return;
    set_volume_locked(percent);
    audio_lock_give();
}

esp_err_t audio_stream_begin(int sample_rate_hz)
{
    if (!codec_dev) return ESP_ERR_INVALID_STATE;
    if (sample_rate_hz < 8000 || sample_rate_hz > 96000) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!audio_lock_take()) return ESP_ERR_TIMEOUT;

    if (stream_open) {  // a new stream replaces the old one
        esp_codec_dev_close(codec_dev);
        stream_open = false;
    }
    // The data interface reconfigures the I2S clock when the rate differs, so
    // one codec device serves both the 24 kHz tones and a 44.1 kHz stream.
    fs.sample_rate = (uint32_t)sample_rate_hz;
    if (esp_codec_dev_open(codec_dev, &fs) != ESP_CODEC_DEV_OK) {
        audio_lock_give();
        ESP_LOGE(TAG, "codec open failed at %d Hz", sample_rate_hz);
        return ESP_FAIL;
    }
    set_volume_locked(settings.audio_volume);
    stream_open = true;
    stream_rate = sample_rate_hz;
    audio_lock_give();
    ESP_LOGI(TAG, "stream at %d Hz", sample_rate_hz);
    return ESP_OK;
}

esp_err_t audio_stream_write(const void *pcm, size_t bytes)
{
    if (!codec_dev || !pcm || bytes == 0) return ESP_ERR_INVALID_ARG;
    if (!audio_lock_take()) return ESP_ERR_TIMEOUT;

    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (stream_open) {
        result = esp_codec_dev_write(codec_dev, (void *)pcm, (int)bytes) ==
                         ESP_CODEC_DEV_OK
                     ? ESP_OK
                     : ESP_FAIL;
    }
    audio_lock_give();
    return result;
}

void audio_stream_end(void)
{
    if (!codec_dev) return;
    if (!audio_lock_take()) return;
    if (stream_open) {
        esp_codec_dev_close(codec_dev);
        stream_open = false;
        stream_rate = 0;
    }
    audio_lock_give();
}

void audio_play_tone_at(int freq_hz, int duration_ms, int volume_pct)
{
    if (!codec_dev || duration_ms <= 0) return;
    if (!audio_lock_take()) {
        ESP_LOGW(TAG, "codec busy, tone skipped");
        return;
    }

    // A running stream keeps the codec open. Writing the tone into it at the
    // stream's rate avoids the click of a re-open and leaves the rate alone;
    // the decoder waits on the lock while the tone plays and resumes after.
    const bool streaming = stream_open;
    const int rate = streaming ? stream_rate : 24000;
    if (!streaming) {
        fs.sample_rate = 24000;  // a previous stream may have changed it
        if (esp_codec_dev_open(codec_dev, &fs) != ESP_CODEC_DEV_OK) {
            audio_lock_give();
            ESP_LOGW(TAG, "codec open failed");
            return;
        }
    }
    set_volume_locked(volume_pct);

    const int total = rate * duration_ms / 1000;
    const int chunk = total < TONE_CHUNK ? total : TONE_CHUNK;
    int16_t *buf = malloc((size_t)chunk * 2 * sizeof(int16_t));
    if (!buf) {
        if (!streaming) esp_codec_dev_close(codec_dev);
        audio_lock_give();
        return;
    }
    for (int done = 0; done < total; done += chunk) {
        const int n = total - done < chunk ? total - done : chunk;
        for (int i = 0; i < n; i++) {
            float t = (float)(done + i) / (float)rate;
            int16_t v =
                (int16_t)(sinf(2.0f * 3.14159265f * freq_hz * t) * 16000.0f);
            buf[i * 2] = v;
            buf[i * 2 + 1] = v;
        }
        esp_codec_dev_write(codec_dev, buf, n * 2 * (int)sizeof(int16_t));
    }
    free(buf);

    if (streaming) {
        set_volume_locked(settings.audio_volume);  // back to the stream level
    } else {
        // Close to disable PA and keep the speaker silent when idle.
        esp_codec_dev_close(codec_dev);
    }
    audio_lock_give();
}