// firmware/main/main.c
//
// CodexBar desk-toy entry point. Boot path:
//   nvs → config_store → display → ui
//   ├─ not provisioned → provision_start()  (SoftAP + captive form; reboots)
//   └─ provisioned      → touch + wifi(STA) + fetch task
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "board_config.h"
#include "display.h"
#include "config_store.h"
#include "ui.h"
#include "app_event.h"
#include "net_wifi.h"
#include "fetch.h"
#include "provision.h"
#if BOARD_HAS_TOUCH
#include "touch.h"
#endif

static const char *TAG = "cbtoy";

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(e);
    }

    config_store_init();   // before display_init (brightness/flip)
    display_init();        // SPI + ILI9341 + LVGL (vendored, proven)
    ui_start();            // LVGL UI task

    if (!config_store_is_provisioned()) {
        ESP_LOGW(TAG, "no credentials — starting captive portal");
        provision_start(); // shows SSID/pass on screen; reboots on form submit
        return;            // idle in AP mode until reboot
    }

    char ssid[CFG_SSID_MAX], pass[CFG_PASS_MAX];
    config_store_get_ssid(ssid, sizeof ssid);
    config_store_get_pass(pass, sizeof pass);

    static QueueHandle_t q;
    q = xQueueCreate(8, sizeof(app_evt_t));
    configASSERT(q);

#if BOARD_HAS_TOUCH
    touch_init(q);         // taps → APP_EVT_TOUCH (refresh / triple-tap reprovision)
#endif
    net_wifi_start(ssid, pass);
    fetch_task_start(q);
    ESP_LOGI(TAG, "running (provisioned)");
}
