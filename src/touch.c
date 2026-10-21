#include "touch.h"
#include "pins.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";
static i2c_master_bus_handle_t bus = NULL;
static i2c_master_dev_handle_t dev = NULL;
static bool available = false;
static touch_tap_cb_t tap_cb = NULL;
static esp_timer_handle_t timer = NULL;

static bool was_touched = false;
static int64_t touch_start_ms = 0;

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, 100);
}

static void poll(void *arg)
{
    if (!available || !dev) return;
    uint8_t buf[6] = {0};
    if (read_regs(0x02, buf, sizeof(buf)) != ESP_OK) return;

    if (buf[0] == 0xFF) buf[0] = 0x00;
    int num = buf[0] & 0x01;
    int x = ((buf[1] & 0x0F) << 8) | buf[2];
    int y = ((buf[3] & 0x0F) << 8) | buf[4];
    (void)x; (void)y;

    int64_t now = esp_timer_get_time() / 1000;
    if (num > 0 && !was_touched) {
        was_touched = true;
        touch_start_ms = now;
    } else if (num == 0 && was_touched) {
        was_touched = false;
        int64_t dur = now - touch_start_ms;
        if (dur < 500 && tap_cb) {
            tap_cb();
        }
    }
}

esp_err_t touch_init(touch_tap_cb_t cb)
{
    tap_cb = cb;

    // Reset pin.
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << PIN_TP_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << PIN_TP_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&int_cfg);

    gpio_set_level(PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(PIN_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = PIN_TP_SDA,
        .scl_io_num = PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "i2c bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TP_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ret = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "add device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t chip_id = 0;
    uint8_t reg = 0xA3;
    ret = i2c_master_transmit_receive(dev, &reg, 1, &chip_id, 1, 100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CST816D not found, non-touch mode");
        i2c_master_bus_rm_device(dev);
        dev = NULL;
        i2c_del_master_bus(bus);
        bus = NULL;
        return ESP_OK;
    }
    ESP_LOGI(TAG, "CST816D chip id 0x%02X", chip_id);
    available = true;

    esp_timer_create_args_t args = {
        .callback = poll,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "touch_poll",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 10 * 1000));
    return ESP_OK;
}

bool touch_is_available(void)
{
    return available;
}