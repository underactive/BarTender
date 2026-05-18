// firmware/main/touch.c
//
// FT6336G capacitive touch driver. Wraps the espressif/esp_lcd_touch_ft5x06
// managed component (FT6336G shares the FT5x06 register layout).
//
// Strategy: poll-based, not interrupt-driven. A dedicated low-priority task
// wakes every 20 ms, asks the driver to refresh its internal cache, and
// checks for a fresh touch-down. This is simpler than wiring the INT line
// into a GPIO ISR, and 50 Hz is more than fast enough for "did the user tap?"
// UX. If power draw becomes a concern, the INT pin can be swapped in later
// as a wakeup source.
//
// Each debounced tap posts APP_EVT_TOUCH to the shared event queue. The
// CodexBar toy uses this to force an immediate stats refresh; a triple-tap
// (detected in fetch_task.c) wipes creds and re-enters provisioning.

#include "touch.h"
#include "board_config.h"

#if BOARD_HAS_TOUCH

#include "app_event.h"
#include "i2c_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "touch";

#define POLL_PERIOD_MS  20     // 50 Hz
#define DEBOUNCE_US     200000 // 200 ms between accepted taps

static QueueHandle_t s_queue;
static esp_lcd_touch_handle_t s_touch;
static int64_t s_last_press_us;
static bool s_was_touching;

static void touch_task(void *arg)
{
    (void)arg;
    while (1) {
        if (esp_lcd_touch_read_data(s_touch) == ESP_OK) {
            uint16_t x[1] = {0};
            uint16_t y[1] = {0};
            uint16_t strength[1] = {0};
            uint8_t count = 0;
            bool pressed = esp_lcd_touch_get_coordinates(s_touch, x, y, strength, &count, 1);

            /* Fire on edge: idle → touching. Ignore continued hold and release. */
            if (pressed && count > 0 && !s_was_touching) {
                int64_t now = esp_timer_get_time();
                if (now - s_last_press_us >= DEBOUNCE_US) {
                    s_last_press_us = now;
                    app_evt_t evt = { .type = APP_EVT_TOUCH };
                    xQueueSend(s_queue, &evt, 0);
                    ESP_LOGD(TAG, "Tap at (%u, %u)", x[0], y[0]);
                }
            }
            s_was_touching = (pressed && count > 0);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

void touch_init(QueueHandle_t evt_queue)
{
    s_queue = evt_queue;
    s_last_press_us = 0;
    s_was_touching = false;

    /* Shared I2C master bus — also used by the ES8311 codec (sound.c). */
    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (!bus) {
        ESP_LOGE(TAG, "Failed to acquire shared I2C bus");
        return;
    }

    /* LCD panel IO (I2C) — the esp_lcd abstraction over the FT5x06 register
     * protocol. The ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG macro supplies the
     * correct slave address, bit widths, and control-byte format. */
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io));

    /* Touch driver.
     *
     * Landscape mapping: the FT6336G reports raw portrait coordinates in a
     * 240x320 coordinate frame. We rotate the LCD panel to landscape (320x240)
     * via ESP-IDF's panel driver, so touch needs the same rotation applied
     * here. swap_xy=1 is the first step; mirror flags may need a flip
     * depending on the physical board orientation. Values below mirror the
     * LCD panel orientation flags in board_config.h — keep them in sync. */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_TOUCH_NATIVE_W,  /* raw panel width — swap_xy flips the axes */
        .y_max = BOARD_TOUCH_NATIVE_H,
        .rst_gpio_num = BOARD_TOUCH_RST_GPIO,
        .int_gpio_num = -1,  /* poll-based; INT not wired for now */
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = {
            .swap_xy  = BOARD_LCD_SWAP_XY,
            .mirror_x = BOARD_LCD_MIRROR_X,
            .mirror_y = BOARD_LCD_MIRROR_Y,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(io, &tp_cfg, &s_touch));

    BaseType_t ret = xTaskCreate(touch_task, "touch", 3072, NULL, 3, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create touch task");
        return;
    }

    ESP_LOGI(TAG, "FT6336G touch initialized (I2C SDA=%d SCL=%d, addr=0x%02x)",
             BOARD_I2C_SDA, BOARD_I2C_SCL, BOARD_TOUCH_I2C_ADDR);
}

#else  /* !BOARD_HAS_TOUCH */

void touch_init(QueueHandle_t evt_queue) { (void)evt_queue; }

#endif
