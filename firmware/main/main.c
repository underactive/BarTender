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

    // Upstash and WiFi are now independent. Open the captive portal if either
    // is missing, OR if a non-destructive triple-tap / self-heal asked for it
    // (take_portal_request clears the one-shot flag FIRST — clear-before-act —
    // so a power loss in the portal can never boot-loop). When Upstash is
    // already set the portal serves the WiFi-only form (keeps the token).
    bool have_upstash = config_store_has_upstash();
    bool have_wifi    = config_store_wifi_count() > 0;
    bool forced       = config_store_take_portal_request();

    if (forced || !have_upstash || !have_wifi) {
        ESP_LOGW(TAG, "captive portal (forced=%d upstash=%d wifi=%d)",
                 forced, have_upstash, have_wifi);
        provision_start(have_upstash);   // WiFi-only form if Upstash present
        return;                          // idle in AP mode until reboot
    }

    static QueueHandle_t q;
    q = xQueueCreate(8, sizeof(app_evt_t));
    configASSERT(q);

#if BOARD_HAS_TOUCH
    touch_init(q);         // taps → APP_EVT_TOUCH (refresh / triple-tap add-network)
#endif
    net_wifi_start_multi();   // scans + autoconnects to a remembered network
    fetch_task_start(q);
    ESP_LOGI(TAG, "running (upstash + %u WiFi)", config_store_wifi_count());
}
