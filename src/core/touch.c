#include "touch.h"
#include <stdlib.h>
#include "pins.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Interrupt-driven touch: the CST816D INT line wakes a dedicated task that
// reads the controller over I2C. Taps and long presses fire on release, so a
// deliberate hold can be interrupted: a finger kept at one spot for
// TOUCH_SETUP_HOLD_MS is the setup gesture instead of a mode switch.
#define TOUCH_DEBOUNCE_MS 20       // coalesce bursty INTs before reading
#define TOUCH_TAP_MAX_MS 500       // hold length that counts as a long press
#define TOUCH_HOLD_POLL_MS 100     // fallback poll rate while holding
#define TOUCH_SETUP_HOLD_MS 5000   // still hold that opens the setup gesture
#define TOUCH_SETUP_SETTLE_MS 400  // time a landing finger gets to calm down
#define TOUCH_SETUP_MOVE_PX 40     // drift away from the settled position
#define TOUCH_SETUP_SLIDE_PX 20    // motion within one poll, catches a swipe

static const char *TAG = "touch";
static i2c_master_bus_handle_t bus = NULL;
static i2c_master_dev_handle_t dev = NULL;
static bool available = false;
static touch_tap_cb_t tap_cb = NULL;
static touch_long_cb_t long_cb = NULL;
static esp_timer_handle_t hold_timer = NULL;
static TaskHandle_t touch_task_handle = NULL;

static touch_setup_cb_t setup_cb = NULL;
static esp_timer_handle_t setup_timer = NULL;
static volatile bool fire_setup = false; // set by the setup timer

static bool was_touched = false;
static int64_t touch_start_ms = 0;
static bool long_fired = false;     // long press used up for this hold
static bool setup_armed = false;    // this hold may still open the setup
static bool setup_fired = false;
static int start_x = 0;         // settled position of this hold
static int start_y = 0;
static int prev_x = 0;          // previous report, catches a swipe
static int prev_y = 0;
static bool settled = false;    // the landing jitter is over
static volatile bool slow_poll = false;

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, 100);
}

static void IRAM_ATTR touch_isr(void *arg)
{
    BaseType_t hpw = pdFALSE;
    if (touch_task_handle) {
        vTaskNotifyGiveFromISR(touch_task_handle, &hpw);
    }
    if (hpw) {
        portYIELD_FROM_ISR();
    }
}

static void hold_timer_cb(void *arg)
{
    // TOUCH_TAP_MAX_MS passed: the long press waits for the release now, but
    // polling has to start so a controller that stopped raising INTs is still
    // noticed.
    slow_poll = true;
}

static void setup_timer_cb(void *arg)
{
    // The finger has been in place for TOUCH_SETUP_HOLD_MS.
    fire_setup = true;
    if (touch_task_handle) {
        xTaskNotifyGive(touch_task_handle);
    }
}

// Hands the setup gesture over to whoever registered it. Kept in the touch
// task, so no callback ever runs in the timer context.
static void fire_setup_gesture(void)
{
    setup_fired = true;
    setup_armed = false;
    long_fired = true; // this hold is spoken for, no mode switch on release
    if (setup_cb) setup_cb();
}

static void handle_touch(void)
{
    if (fire_setup) {
        fire_setup = false;
        if (was_touched && setup_armed && !setup_fired) {
            fire_setup_gesture();
        }
    }

    uint8_t buf[6] = {0};
    if (read_regs(0x02, buf, sizeof(buf)) != ESP_OK) return;

    if (buf[0] == 0xFF) buf[0] = 0x00;
    int num = buf[0] & 0x01;

    // Finger position: the controller puts the high nibble of X and Y in
    // registers 0x03 and 0x05, the low byte in the register after each.
    int x = ((buf[1] & 0x0F) << 8) | buf[2];
    int y = ((buf[3] & 0x0F) << 8) | buf[4];

    int64_t now = esp_timer_get_time() / 1000;
    if (num > 0 && !was_touched) {
        was_touched = true;
        long_fired = false;
        setup_fired = false;
        setup_armed = setup_cb != NULL;
        start_x = x;
        start_y = y;
        prev_x = x;
        prev_y = y;
        settled = false;
        touch_start_ms = now;
        ESP_LOGI(TAG, "touch at %d,%d", x, y);
        esp_timer_stop(hold_timer);
        esp_timer_start_once(hold_timer, TOUCH_TAP_MAX_MS * 1000);
        if (setup_armed) {
            esp_timer_stop(setup_timer);
            esp_timer_start_once(setup_timer, TOUCH_SETUP_HOLD_MS * 1000);
        }
    } else if (num > 0 && was_touched) {
        if (!settled && now - touch_start_ms >= TOUCH_SETUP_SETTLE_MS) {
            // Measure the rest of the hold against the position the finger
            // ended up at, not the one it landed on: the first reports of a
            // fresh touch still jump around.
            start_x = x;
            start_y = y;
            settled = true;
        }
        if (setup_armed && settled &&
            (abs(x - start_x) > TOUCH_SETUP_MOVE_PX ||
             abs(y - start_y) > TOUCH_SETUP_MOVE_PX ||
             abs(x - prev_x) > TOUCH_SETUP_SLIDE_PX ||
             abs(y - prev_y) > TOUCH_SETUP_SLIDE_PX)) {
            setup_armed = false;
            esp_timer_stop(setup_timer);
            ESP_LOGI(TAG, "setup gesture cancelled, finger moved "
                          "(dx %d/%d, dy %d/%d)",
                     x - start_x, x - prev_x, y - start_y, y - prev_y);
        }
        prev_x = x;
        prev_y = y;
    } else if (num == 0 && was_touched) {
        was_touched = false;
        slow_poll = false;
        esp_timer_stop(hold_timer);
        esp_timer_stop(setup_timer);
        int64_t dur = now - touch_start_ms;
        // Safety net for a setup hold whose timer callback lost the race
        // against the release.
        if (fire_setup || (setup_armed && dur >= TOUCH_SETUP_HOLD_MS)) {
            fire_setup = false;
            if (setup_armed && !setup_fired) fire_setup_gesture();
        }
        if (!long_fired) {
            if (dur < TOUCH_TAP_MAX_MS && tap_cb) {
                tap_cb();
            } else if (dur >= TOUCH_TAP_MAX_MS && long_cb) {
                long_cb();
            }
        }
        long_fired = false;
        setup_armed = false;
        setup_fired = false;
        settled = false;
    }
}

static void touch_task(void *arg)
{
    while (1) {
        // Woken by an INT, the hold-fallback timer or a slow-poll tick.
        TickType_t wait = slow_poll ? pdMS_TO_TICKS(TOUCH_HOLD_POLL_MS)
                                    : portMAX_DELAY;
        ulTaskNotifyTake(pdTRUE, wait);

        vTaskDelay(pdMS_TO_TICKS(TOUCH_DEBOUNCE_MS));
        while (ulTaskNotifyTake(pdTRUE, 0) != 0) {
        } // drain coalesced INTs
        handle_touch();
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
        .intr_type = GPIO_INTR_NEGEDGE,
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

    esp_timer_create_args_t hold_args = {
        .callback = hold_timer_cb,
        .name = "tp_hold",
    };
    ESP_ERROR_CHECK(esp_timer_create(&hold_args, &hold_timer));
    esp_timer_create_args_t setup_args = {
        .callback = setup_timer_cb,
        .name = "tp_setup",
    };
    ESP_ERROR_CHECK(esp_timer_create(&setup_args, &setup_timer));

    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "isr service failed: %s", esp_err_to_name(ret));
    }
    gpio_isr_handler_add(PIN_TP_INT, touch_isr, NULL);

    if (xTaskCreate(touch_task, "touch", 3072, NULL, 5, &touch_task_handle)
        != pdPASS) {
        ESP_LOGE(TAG, "touch task creation failed");
        available = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void touch_set_long_press_cb(touch_long_cb_t cb)
{
    long_cb = cb;
}

void touch_set_setup_cb(touch_setup_cb_t cb)
{
    setup_cb = cb;
}

bool touch_is_available(void)
{
    return available;
}