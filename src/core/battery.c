#include "battery.h"

#include "audio.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pins.h"

#if __has_include("config.h")
#include "config.h"
#endif

// Polarity of the charging input: the board holds the line high while the
// charger is active, which is what the vendor firmware assumes.
#ifndef BATTERY_CHARGING_ACTIVE_HIGH
#define BATTERY_CHARGING_ACTIVE_HIGH 1
#endif

// ADC samples averaged per reading.
#ifndef BATTERY_SAMPLE_COUNT
#define BATTERY_SAMPLE_COUNT 3
#endif

// Minimum age of a cached reading in seconds. The battery barely moves, so
// the ADC is sampled by a periodic timer at most once per interval and the
// display simply reads the cache as often as it likes.
#ifndef BATTERY_REFRESH_SEC
#define BATTERY_REFRESH_SEC 300
#endif

// Charge level (percent) at or below which a short warning beep sounds on
// every refresh. Set to 0 to disable the warning.
#ifndef BATTERY_WARN_PCT
#define BATTERY_WARN_PCT 10
#endif

// Tone of that warning: deliberately short and quiet, it is meant to inform
// rather than to startle.
#define BATTERY_BEEP_FREQ_HZ 1500
#define BATTERY_BEEP_MS 120

// Output volume of the warning beep in percent (0-100).
#ifndef BATTERY_BEEP_VOLUME
#define BATTERY_BEEP_VOLUME 30
#endif

static const char *TAG = "battery";

// Raw 12-bit ADC1 readings at PIN_BAT_ADC (12 dB attenuation) against the
// board's battery divider, mapped to the charge level in percent. The steps
// are not evenly spaced in voltage because of the divider and the attenuation
// curve; the table matches the vendor firmware for this board.
static const struct {
    uint16_t adc;
    uint8_t level;
} level_table[] = {
    {1980,   0}, {2081,  20}, {2163,  40},
    {2250,  60}, {2340,  80}, {2480, 100},
};
#define LEVEL_TABLE_LEN (sizeof(level_table) / sizeof(level_table[0]))

static adc_oneshot_unit_handle_t adc_unit;
static adc_channel_t adc_chan;
static bool ready = false;
static int last_charge_level = -1;
static esp_timer_handle_t refresh_timer = NULL;

static int cached_level = -1;
static bool cached_charging = false;
static int64_t cached_at_us = 0;

static void refresh_timer_cb(void *arg);

esp_err_t battery_init(void)
{
    adc_unit_t unit;
    adc_channel_t channel;
    esp_err_t err = adc_oneshot_io_to_channel(PIN_BAT_ADC, &unit, &channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO %d is not an ADC channel", PIN_BAT_ADC);
        return err;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = unit,
    };
    err = adc_oneshot_new_unit(&unit_cfg, &adc_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(adc_unit, channel, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(adc_unit);
        adc_unit = NULL;
        return err;
    }

    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << PIN_BAT_CHG,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "charge pin config failed: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(adc_unit);
        adc_unit = NULL;
        return err;
    }

    adc_chan = channel;
    ready = true;
    ESP_LOGI(TAG, "battery monitor ready (ADC GPIO %d, charge pin GPIO %d)",
             PIN_BAT_ADC, PIN_BAT_CHG);
    // Effective settings after config.h has been applied, so a wrong value
    // in the local config shows up here instead of having to be guessed.
    ESP_LOGI(TAG, "settings: warn<=%d%%, refresh %ds, beep vol %d%%, charge pin active %s",
             BATTERY_WARN_PCT, BATTERY_REFRESH_SEC, BATTERY_BEEP_VOLUME,
             BATTERY_CHARGING_ACTIVE_HIGH ? "high" : "low");

    // Sampling on a timer keeps the reading and the warning beep on a fixed
    // cadence even while a mode that never draws the clock is active.
    esp_timer_create_args_t timer_cfg = {
        .callback = refresh_timer_cb,
        .name = "battery_refresh",
    };
    if (esp_timer_create(&timer_cfg, &refresh_timer) == ESP_OK) {
        esp_timer_start_periodic(refresh_timer,
                                 (uint64_t)BATTERY_REFRESH_SEC * 1000000ULL);
    } else {
        ESP_LOGW(TAG, "refresh timer unavailable, sampling on demand");
    }
    return ESP_OK;
}

// Samples the ADC and the charging input, then stores the result in the
// cache. Runs at most once per BATTERY_REFRESH_SEC.
static void battery_refresh(void)
{
    uint32_t sum = 0;
    uint32_t avg = 0;
    bool sampled = true;
    for (int i = 0; i < BATTERY_SAMPLE_COUNT; i++) {
        int raw = 0;
        if (adc_oneshot_read(adc_unit, adc_chan, &raw) != ESP_OK) {
            ESP_LOGW(TAG, "ADC read failed");
            sampled = false;
            break;
        }
        sum += (uint32_t)raw;
        if (i + 1 < BATTERY_SAMPLE_COUNT) vTaskDelay(pdMS_TO_TICKS(2));
    }

    if (sampled) {
        avg = sum / BATTERY_SAMPLE_COUNT;

        if (avg <= level_table[0].adc) {
            cached_level = level_table[0].level;
        } else if (avg >= level_table[LEVEL_TABLE_LEN - 1].adc) {
            cached_level = level_table[LEVEL_TABLE_LEN - 1].level;
        } else {
            for (size_t i = 0; i + 1 < LEVEL_TABLE_LEN; i++) {
                if (avg < level_table[i + 1].adc) {
                    uint32_t span = level_table[i + 1].adc - level_table[i].adc;
                    uint32_t delta = avg - level_table[i].adc;
                    int base = level_table[i].level;
                    int step = level_table[i + 1].level - base;
                    cached_level = base + (int)((delta * (uint32_t)step + span / 2) / span);
                    break;
                }
            }
        }
    }

    int pin_level = gpio_get_level(PIN_BAT_CHG);
#if BATTERY_CHARGING_ACTIVE_HIGH
    bool charging = (pin_level == 1);
#else
    bool charging = (pin_level == 0);
#endif
    if (pin_level != last_charge_level) {
        last_charge_level = pin_level;
        ESP_LOGI(TAG, "charging: %s (pin level %d)",
                 charging ? "yes" : "no", pin_level);
    }
    cached_charging = charging;
    cached_at_us = esp_timer_get_time();

    // One line per refresh: keeps the raw ADC value visible for diagnosing a
    // wrong percentage without flooding the log.
    if (sampled) {
        ESP_LOGI(TAG, "level %d%% (raw %u, charging %s)",
                 cached_level, (unsigned)avg, charging ? "yes" : "no");
    }

#if BATTERY_WARN_PCT > 0
    if (sampled && cached_level >= 0 && cached_level <= BATTERY_WARN_PCT) {
        ESP_LOGW(TAG, "battery low: %d%%", cached_level);
        audio_play_tone_at(BATTERY_BEEP_FREQ_HZ, BATTERY_BEEP_MS,
                           BATTERY_BEEP_VOLUME);
    }
#endif
}

static void refresh_timer_cb(void *arg)
{
    (void)arg;
    battery_refresh();
}

static bool cache_is_fresh(void)
{
    if (cached_at_us == 0) return false;
    return esp_timer_get_time() - cached_at_us <
           (int64_t)BATTERY_REFRESH_SEC * 1000000LL;
}

int battery_level(void)
{
    if (!ready) return -1;
    if (!cache_is_fresh()) battery_refresh();
    return cached_level;
}

bool battery_is_charging(void)
{
    if (!ready) return false;
    if (!cache_is_fresh()) battery_refresh();
    return cached_charging;
}
