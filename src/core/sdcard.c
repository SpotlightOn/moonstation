#include "sdcard.h"
#include "pins.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"

static const char *TAG = "sdcard";
static sdmmc_card_t *card = NULL;
static bool mounted = false;

esp_err_t sdcard_init(void)
{
    if (mounted) {
        return ESP_OK;
    }

    // D3/CS must be high before init (matches the reference firmware).
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_SD_D3,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(PIN_SD_D3, 1);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = PIN_SD_CLK;
    slot_config.cmd = PIN_SD_CMD;
    slot_config.d0 = PIN_SD_D0;
    slot_config.d3 = PIN_SD_D3;

    ESP_LOGI(TAG, "mounting SD card");
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(ret));
        mounted = false;
        return ret;
    }
    mounted = true;
    sdmmc_card_print_info(stdout, card);
    return ESP_OK;
}

esp_err_t sdcard_deinit(void)
{
    if (!mounted) {
        return ESP_OK;
    }
    esp_err_t ret = esp_vfs_fat_sdcard_unmount("/sdcard", card);
    if (ret == ESP_OK) {
        mounted = false;
        card = NULL;
    }
    return ret;
}

bool sdcard_is_mounted(void)
{
    return mounted;
}