#include "display.h"
#include "pins.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"

#if __has_include("config.h")
#include "config.h"
#endif

#ifndef LCD_SPI_HZ
#define LCD_SPI_HZ 40000000
#endif

static const char *TAG = "display";
static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "init SPI bus");
    spi_bus_config_t buscfg = GC9A01_PANEL_BUS_SPI_CONFIG(PIN_LCD_SCLK, PIN_LCD_MOSI,
                                                          240 * 240 * 2);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "init panel IO");
    esp_lcd_panel_io_spi_config_t io_config = GC9A01_PANEL_IO_SPI_CONFIG(PIN_LCD_CS, PIN_LCD_DC, NULL, NULL);
    io_config.pclk_hz = LCD_SPI_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "init GC9A01 panel");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    // Vendor-specific tuning (from the reference firmware).
    uint8_t d62[] = { 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70 };
    esp_lcd_panel_io_tx_param(io_handle, 0x62, d62, sizeof(d62));
    uint8_t d63[] = { 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70 };
    esp_lcd_panel_io_tx_param(io_handle, 0x63, d63, sizeof(d63));
    uint8_t d36[] = { 0x48 };
    esp_lcd_panel_io_tx_param(io_handle, 0x36, d36, sizeof(d36));
    uint8_t dc3[] = { 0x1F };
    esp_lcd_panel_io_tx_param(io_handle, 0xC3, dc3, sizeof(dc3));
    uint8_t dc4[] = { 0x1F };
    esp_lcd_panel_io_tx_param(io_handle, 0xC4, dc4, sizeof(dc4));

    ESP_LOGI(TAG, "display ready");
    return ESP_OK;
}

esp_lcd_panel_handle_t display_get_panel(void)
{
    return panel_handle;
}

void display_backlight_on(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(PIN_LCD_BL, 0); // active low
}

void display_backlight_off(void)
{
    gpio_set_level(PIN_LCD_BL, 1);
}