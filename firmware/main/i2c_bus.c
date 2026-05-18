// firmware/main/i2c_bus.c
#include "i2c_bus.h"

#if BOARD_HAS_I2C_BUS

#include "esp_log.h"

static const char *TAG = "i2c_bus";
static i2c_master_bus_handle_t s_bus = NULL;

i2c_master_bus_handle_t i2c_bus_get(void)
{
    if (s_bus) return s_bus;

    i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = 0,
        .scl_io_num = BOARD_I2C_SCL,
        .sda_io_num = BOARD_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        s_bus = NULL;
        return NULL;
    }
    ESP_LOGI(TAG, "I2C master bus ready (SDA=%d SCL=%d)", BOARD_I2C_SDA, BOARD_I2C_SCL);
    return s_bus;
}

#endif
