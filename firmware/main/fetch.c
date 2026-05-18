// firmware/main/fetch.c
#include "fetch.h"
#include "app_event.h"
#include "config_store.h"
#include "net_wifi.h"
#include "upstash.h"
#include "stats_model.h"
#include "ui.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "fetch";
static QueueHandle_t s_q;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static bool do_fetch(const char *url, const char *key, const char *tok)
{
    ui_set_status("fetching…");
    static char body[2560];                 // live payload ~450 B; headroom
    size_t bl = 0;
    upstash_status_t us = upstash_get(url, key, tok, body, sizeof body, &bl);
    if (us != UPSTASH_OK) {
        char m[72];
        snprintf(m, sizeof m, "fetch error: %s", upstash_status_str(us));
        ui_set_status(m);
        return false;
    }
    stats_t st;
    switch (stats_model_parse(body, &st)) {
        case STATS_PARSE_OK:
            ui_set_status("WiFi OK");
            ui_set_stats(&st, now_ms());
            return true;
        case STATS_PARSE_NO_DATA:
            ui_set_status("waiting for publisher…");
            return true;                     // reachable; just no value yet
        default:
            ui_set_status("bad data from store");
            return false;
    }
}

static void reprovision(void)
{
    ESP_LOGW(TAG, "triple-tap — clearing creds, rebooting to portal");
    ui_set_status("re-provisioning… rebooting");
    config_store_clear_provisioning();
    vTaskDelay(pdMS_TO_TICKS(900));
    esp_restart();
}

static void fetch_task(void *arg)
{
    (void)arg;
    char url[CFG_URL_MAX], key[CFG_KEY_MAX], tok[CFG_TOKEN_MAX];
    config_store_get_upstash_url(url, sizeof url);
    config_store_get_upstash_key(key, sizeof key);
    config_store_get_upstash_token(tok, sizeof tok);

    while (!net_wifi_is_connected()) {
        ui_set_status("WiFi: connecting…");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    int64_t taps[3] = { 0, 0, 0 };
    int tc = 0;
    bool refresh_now = true;

    for (;;) {
        if (refresh_now) {
            bool ok = net_wifi_is_connected() ? do_fetch(url, key, tok)
                                              : (ui_set_status("WiFi: reconnecting…"), false);
            refresh_now = false;
            int wait_s = ok ? FETCH_INTERVAL_S : FETCH_RETRY_S;
            int64_t deadline = now_ms() + (int64_t)wait_s * 1000;

            // Wait for the deadline, but wake early on a tap.
            while (!refresh_now) {
                int64_t rem = deadline - now_ms();
                if (rem <= 0) { refresh_now = true; break; }
                app_evt_t ev;
                TickType_t to = pdMS_TO_TICKS(rem > 1000 ? 1000 : (int)rem);
                if (xQueueReceive(s_q, &ev, to) == pdTRUE && ev.type == APP_EVT_TOUCH) {
                    int64_t t = now_ms();
                    taps[0] = taps[1]; taps[1] = taps[2]; taps[2] = t;
                    if (tc < 3) tc++;
                    // Audit QA§MED: require 3 DELIBERATE rapid taps — all
                    // within 1200 ms AND each consecutive gap ≤ 600 ms — so
                    // stray FT6336 touch noise over weeks of uptime cannot
                    // silently factory-reset. touch.c debounces at 200 ms, so
                    // an intentional triple-tap comfortably fits this window.
                    if (tc == 3 &&
                        (t - taps[0]) <= 1200 &&
                        (taps[1] - taps[0]) <= 600 &&
                        (taps[2] - taps[1]) <= 600) {
                        reprovision();                        // no return
                    }
                    refresh_now = true;                       // single tap → refresh
                }
            }
        }
    }
}

void fetch_task_start(QueueHandle_t evt_q)
{
    s_q = evt_q;
    xTaskCreate(fetch_task, "fetch", 6144, NULL, 4, NULL);
}
