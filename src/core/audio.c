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
#include "math.h"

static const char *TAG = "audio";
static esp_codec_dev_handle_t codec_dev = NULL;
static i2s_chan_handle_t tx_handle = NULL;
static esp_codec_dev_sample_info_t fs = {
    .sample_rate = 24000,
    .channel = 2,
    .bits_per_sample = 16,
};

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

void audio_play_tone_at(int freq_hz, int duration_ms, int volume_pct)
{
    if (!codec_dev) return;
    if (esp_codec_dev_open(codec_dev, &fs) != 0) {
        ESP_LOGW(TAG, "codec open failed");
        return;
    }
    if (volume_pct < 0) volume_pct = 0;
    if (volume_pct > 100) volume_pct = 100;
    esp_codec_dev_set_out_vol(codec_dev, volume_pct);

    int samples = 24000 * duration_ms / 1000;
    int16_t *buf = malloc(samples * 2 * sizeof(int16_t));
    if (!buf) {
        esp_codec_dev_close(codec_dev);
        return;
    }
    for (int i = 0; i < samples; i++) {
        float t = (float)i / 24000.0f;
        int16_t v = (int16_t)(sinf(2.0f * 3.14159265f * freq_hz * t) * 16000.0f);
        buf[i * 2] = v;
        buf[i * 2 + 1] = v;
    }
    esp_codec_dev_write(codec_dev, buf, samples * 2 * sizeof(int16_t));
    free(buf);

    // Close to disable PA and keep the speaker silent when idle.
    esp_codec_dev_close(codec_dev);
}