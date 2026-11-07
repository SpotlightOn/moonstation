// Boot partition switch between the two firmwares in the flash. moonstation
// runs from factory, xiaozhi-esp32 from ota_0; otadata decides which one the
// bootloader starts. See docs/dual-boot.md.
#include "bootswitch.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "audio.h"
#include "pins.h"

static const char *TAG = "bootswitch";

// A hold on the BOOT button, not a tap: the button sits next to the USB port
// and is easy to brush, and leaving the firmware must be deliberate.
#define BOOT_HOLD_MS 2000
#define BOOT_POLL_MS 50

// The label is checked as well as the subtype: a table without ota_0 (an older
// single-app layout) must fail here instead of picking some other partition.
static const esp_partition_t *xiaozhi_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0");
}

bool bootswitch_xiaozhi_available(void)
{
    const esp_partition_t *part = xiaozhi_partition();
    if (part == NULL) return false;

    // The slot is erased flash until xiaozhi has been written once; reading its
    // app description is the cheap way to tell.
    esp_app_desc_t desc;
    return esp_ota_get_partition_description(part, &desc) == ESP_OK;
}

bool bootswitch_to_xiaozhi(void)
{
    const esp_partition_t *part = xiaozhi_partition();
    if (part == NULL) {
        ESP_LOGW(TAG, "partition table has no ota_0 slot");
        return false;
    }

    esp_app_desc_t desc;
    esp_err_t err = esp_ota_get_partition_description(part, &desc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ota_0 holds no image (%s)", esp_err_to_name(err));
        return false;
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot select ota_0: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "next boot: %.*s %.*s from ota_0",
             (int)sizeof(desc.project_name), desc.project_name,
             (int)sizeof(desc.version), desc.version);
    return true;
}

static void button_task(void *arg)
{
    (void)arg;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_BOOT_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    bool down_before = false;
    bool fired = false;      // this hold already did its job
    int64_t down_since = 0;

    while (true) {
        bool down = gpio_get_level(PIN_BOOT_BTN) == 0;
        int64_t now_ms = esp_timer_get_time() / 1000;

        if (!down) {
            down_before = false;
        } else if (!down_before) {
            down_before = true;
            fired = false;
            down_since = now_ms;
        } else if (!fired && now_ms - down_since >= BOOT_HOLD_MS) {
            fired = true;   // one switch per hold, even if the finger stays
            if (!bootswitch_xiaozhi_available()) {
                ESP_LOGW(TAG, "BOOT button held, but ota_0 holds no image");
            } else if (bootswitch_to_xiaozhi()) {
                audio_play_tone(880, 60);
                vTaskDelay(pdMS_TO_TICKS(150));
                esp_restart();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_MS));
    }
}

void bootswitch_start_button_monitor(void)
{
    if (xTaskCreate(button_task, "boot_btn", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the BOOT button task");
    }
}
