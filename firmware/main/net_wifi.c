// firmware/main/net_wifi.c
#include "net_wifi.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "wifi";

static volatile bool s_connected = false;
static int s_backoff_idx = 0;
static const int s_backoff_s[] = { 5, 15, 60 };   // capped reconnect schedule
static esp_timer_handle_t s_retry_timer;

static void retry_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "reconnecting...");
    esp_wifi_connect();
}

static void arm_retry(void)
{
    int s = s_backoff_s[s_backoff_idx];
    if (s_backoff_idx < (int)(sizeof s_backoff_s / sizeof s_backoff_s[0]) - 1)
        s_backoff_idx++;
    ESP_LOGW(TAG, "link down — retry in %ds", s);
    esp_timer_start_once(s_retry_timer, (uint64_t)s * 1000000ULL);
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        arm_retry();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_backoff_idx = 0;            // reset backoff on success
        ESP_LOGI(TAG, "connected, got IP");
    }
}

bool net_wifi_is_connected(void) { return s_connected; }

void net_wifi_start(const char *ssid, const char *pass)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi, NULL, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid ? ssid : "", sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, pass ? pass : "", sizeof wc.sta.password);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    const esp_timer_create_args_t targs = { .callback = &retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "STA starting for SSID \"%s\"", ssid ? ssid : "");
}
