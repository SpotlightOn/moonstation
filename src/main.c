#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "display.h"
#include "audio.h"
#include "slideshow.h"

#if __has_include("config.h")
#include "config.h"
#endif

#ifndef SLIDE_INTERVAL_MS
#define SLIDE_INTERVAL_MS 10000
#endif

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

    ESP_LOGI(TAG, "moonstation boot");
    ESP_LOGI(TAG, "chip: %s, cores=%d, flash=%lu MB",
             CONFIG_IDF_TARGET, chip_info.cores, (unsigned long)(flash_size / (1024 * 1024)));
    ESP_LOGI(TAG, "slide interval: %d ms", SLIDE_INTERVAL_MS);

    ESP_ERROR_CHECK(display_init());
    display_backlight_on();

    // Audio init is best-effort: a failure must not block the slideshow.
    if (audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "audio init failed");
    }

    slideshow_run();
}