#include "display.h"
#include "pins.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "settings.h"

static const char *TAG = "display";
static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;

esp_err_t display_init(void)
{
    // Coming out of deep sleep the pads are still latched by the holds taken
    // in display_deep_sleep_prepare(); release them before reconfiguring.
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(PIN_LCD_MOSI);
    gpio_hold_dis(PIN_LCD_SCLK);
    gpio_hold_dis(PIN_LCD_CS);
    gpio_hold_dis(PIN_LCD_DC);
    gpio_hold_dis(PIN_LCD_RST);
    gpio_hold_dis(PIN_LCD_BL);

    // Park the backlight off immediately: a deep sleep wake-up inside the
    // sleep window must stay dark from the very first millisecond. The pad
    // becomes an LEDC output again in display_backlight_init().
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level((gpio_num_t)PIN_LCD_BL, 1); // active low: off

    ESP_LOGI(TAG, "init SPI bus");
    spi_bus_config_t buscfg = GC9A01_PANEL_BUS_SPI_CONFIG(PIN_LCD_SCLK, PIN_LCD_MOSI,
                                                          240 * 240 * 2);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "init panel IO");
    esp_lcd_panel_io_spi_config_t io_config = GC9A01_PANEL_IO_SPI_CONFIG(PIN_LCD_CS, PIN_LCD_DC, NULL, NULL);
    io_config.pclk_hz = settings.lcd_spi_hz;
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

// The ESP32-S3 decoder writes with SIMD instructions and requires a 16-byte
// aligned output buffer, otherwise the first pixels of a row land in the wrong
// column.
static uint16_t frame_buffer[240 * 240] __attribute__((aligned(16)));

uint16_t *display_get_framebuffer(void)
{
    return frame_buffer;
}

#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL LEDC_CHANNEL_0

void display_backlight_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    // Active low: 100% brightness -> 0% duty, 0% brightness -> 100% duty.
    int duty = (100 - percent) * ((1 << 13) - 1) / 100;
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

void display_backlight_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tcfg);
    ledc_channel_config_t ccfg = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .duty = (1 << 13) - 1,
        .hpoint = 0,
    };
    ledc_channel_config(&ccfg); // 100% duty on an active low pad: off
}

void display_backlight_on(void)
{
    display_backlight_init();
    display_backlight_set_brightness(settings.backlight_full_pct);
}

void display_backlight_off(void)
{
    display_backlight_set_brightness(0);
}

void display_deep_sleep_prepare(void)
{
    // Backlight fully off first: LEDC stops switching in deep sleep and the
    // pad would otherwise float at an undefined brightness (it is held in
    // this off level below).
    display_backlight_set_brightness(0);

    // Sleep the panel: it draws microamps instead of milliamps and shows no
    // meaningful image without the backlight anyway. display_init() performs
    // a hardware reset plus the full init sequence after the next wake-up,
    // which brings the panel back from sleep mode.
    esp_lcd_panel_io_tx_param(io_handle, 0x10, NULL, 0); // GC9A01 sleep in

    // Latch every pad the panel listens on. Deep sleep turns the digital
    // pads into high-Z inputs; without the holds the floating SPI lines
    // would feed noise into the panel.
    gpio_hold_en(PIN_LCD_MOSI);
    gpio_hold_en(PIN_LCD_SCLK);
    gpio_hold_en(PIN_LCD_CS);
    gpio_hold_en(PIN_LCD_DC);
    gpio_hold_en(PIN_LCD_RST);
    gpio_hold_en(PIN_LCD_BL);
    gpio_deep_sleep_hold_en();

    // The PA enable pad is latched low (off) by audio_init(); see there.
}
