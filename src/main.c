#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "display.h"
#include "audio.h"
#include "power.h"
#include "settings.h"
#include "slideshow.h"

static const char *TAG = "moonstation";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    settings_init(); // after nvs_flash_init, before anything reads a value
    power_init();    // one place for the clock ceiling every module uses

    ESP_LOGI(TAG, "moonstation boot");
    ESP_LOGI(TAG, "chip: %s, cores=%d, flash=%lu MB",
             CONFIG_IDF_TARGET, chip_info.cores, (unsigned long)(flash_size / (1024 * 1024)));
    ESP_LOGI(TAG, "slide interval: %lu ms", (unsigned long)settings.slide_interval_ms);

    ESP_ERROR_CHECK(display_init());
    if (esp_sleep_get_wakeup_causes() & (1 << ESP_SLEEP_WAKEUP_TIMER)) {
        // Deep sleep wake-up: keep the display dark until the mode manager
        // decides otherwise, so a wake-up inside the sleep window cannot
        // flash at full brightness. The LEDC channel is configured, just
        // parked at 0%, so interactions still light it up.
        display_backlight_init();
    } else {
        display_backlight_on();
    }

    // Audio init is best-effort: a failure must not block the slideshow.
    if (audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "audio init failed");
    }

    slideshow_run();
}