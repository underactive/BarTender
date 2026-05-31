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
    ui_set_status("fetching...");
    // v2 payload (Claude cost block + ~31-day history) is ~1-2 KB escaped vs
    // the old ~450 B; size generously. Oversize still fails safe via
    // upstash.c's response-too-big guard -> "bad data from store".
    static char body[4096];
    size_t bl = 0;
    upstash_status_t us = upstash_get(url, key, tok, body, sizeof body, &bl);
    if (us != UPSTASH_OK) {
        char m[72];
        snprintf(m, sizeof m, "fetch error: %s", upstash_status_str(us));
        ui_set_status(m);
        return false;
    }
    // static: stats_t grew ~1.9 KB with the v2 cost block; keep it off the
    // 6 KB fetch-task stack. Single fetch task, copied into ui.c under mutex.
    static stats_t st;
    switch (stats_model_parse(body, &st)) {
        case STATS_PARSE_OK:
            ui_set_status("WiFi OK");
            ui_set_stats(&st, now_ms());
            return true;
        case STATS_PARSE_NO_DATA:
            ui_set_status("waiting for publisher...");
            return true;                     // reachable; just no value yet
        default:
            ui_set_status("bad data from store");
            return false;
    }
}

// NON-DESTRUCTIVE re-provision. Sets a one-shot NVS flag and reboots; the next
// boot opens the captive portal to ADD a network while KEEPING all remembered
// WiFi + Upstash. (config_store_request_portal() only sets "fprov"; nothing is
// erased.) Replaces the old reprovision() that wiped every credential.
static void enter_portal(void)
{
    // Audit QA§P2-3: a second long-press inside the 900 ms pre-restart window
    // would re-enter this; the guard makes it idempotent so the request/
    // delay/restart isn't stacked. fetch_task is the only caller, so a plain
    // static is sufficient (no concurrency here).
    static bool s_porting;
    if (s_porting) return;
    s_porting = true;
    ESP_LOGW(TAG, "opening setup portal (all creds kept)");
    ui_set_status("opening setup... rebooting");
    config_store_request_portal();
    vTaskDelay(pdMS_TO_TICKS(900));
    esp_restart();
}

// Block until the first association, BUT keep servicing the event queue so the
// long-press add-network gesture works even when no remembered SSID is in
// range. Audit Reliability§HIGH: the OLD code spun in a bare `while(!connected)
// vTaskDelay` here, so the queue was never drained — input piled up unread and
// the ONLY on-device recovery was unreachable behind the very failure it
// recovers from. Self-heal: a relocated toy nobody is there to long-press
// auto-opens the add-network portal — but ONLY once net_wifi has confirmed
// (>= 2 scan sweeps) that ZERO remembered networks are in range, AND the
// grace period elapsed. That gate stops a slow multi-network sweep or a
// wrong-password auth-retry loop from spuriously popping the portal. It is
// NON-destructive (config_store_request_portal keeps all WiFi + Upstash), and
// scoped to the INITIAL connect only — once associated, a later outage just
// rescans (net_wifi handles roaming; nothing is ever wiped).
static void wait_for_first_link(void)
{
    int64_t deadline = now_ms() + (int64_t)CONNECT_GRACE_S * 1000;
    while (!net_wifi_is_connected()) {
        ui_set_status("WiFi: connecting...");
        if (now_ms() >= deadline && net_wifi_no_known_network()) {
            ESP_LOGW(TAG, "no known network in range for %ds — open portal",
                     CONNECT_GRACE_S);
            enter_portal();                           // no return
        }
        app_evt_t ev;
        if (xQueueReceive(s_q, &ev, pdMS_TO_TICKS(500)) != pdTRUE)
            continue;                                 // 500 ms status/deadline tick
        // Nav owns every event; only a LONG_PRESS on the summary pierces
        // through (PASS) → open the add-network portal (never returns).
        if (ui_handle_input(&ev) == UI_INPUT_PASS &&
            ev.type == APP_EVT_LONG_PRESS)
            enter_portal();
    }
}

static void fetch_task(void *arg)
{
    (void)arg;
    char url[CFG_URL_MAX], key[CFG_KEY_MAX], tok[CFG_TOKEN_MAX];
    config_store_get_upstash_url(url, sizeof url);
    config_store_get_upstash_key(key, sizeof key);
    config_store_get_upstash_token(tok, sizeof tok);

    wait_for_first_link();

    bool refresh_now = true;
    int  fail_streak = 0;   // consecutive failures, drives the retry backoff

    for (;;) {
        if (refresh_now) {
            bool ok = net_wifi_is_connected() ? do_fetch(url, key, tok)
                                              : (ui_set_status("WiFi: reconnecting..."), false);
            refresh_now = false;
            // Audit (Backend§MED): on success poll at the steady cadence; on
            // consecutive failures back off exponentially (20,40,80,160,…) up to
            // FETCH_RETRY_MAX_S so a down/erroring store isn't hammered at a
            // fixed rate. Reset to fast retry on the first success.
            int wait_s;
            if (ok) {
                fail_streak = 0;
                wait_s = FETCH_INTERVAL_S;
            } else {
                if (fail_streak < 5) fail_streak++;   // cap shift at 2^4 = 16x
                wait_s = FETCH_RETRY_S << (fail_streak - 1);
                if (wait_s > FETCH_RETRY_MAX_S) wait_s = FETCH_RETRY_MAX_S;
            }
            int64_t deadline = now_ms() + (int64_t)wait_s * 1000;

            // Wait out the fetch interval, servicing input meanwhile so the
            // nav stays responsive between polls (scroll / open page / cycle
            // Cost↔Limit / back are all handled inside ui_handle_input, which
            // flags its own redraw). Refresh is now purely deadline-driven —
            // the nav consumes taps, so there is no tap-to-refresh anymore;
            // only a summary LONG_PRESS pierces through (PASS) to open the
            // add-network portal (never returns).
            while (!refresh_now) {
                int64_t rem = deadline - now_ms();
                if (rem <= 0) { refresh_now = true; break; }
                app_evt_t ev;
                TickType_t to = pdMS_TO_TICKS(rem > 1000 ? 1000 : (int)rem);
                if (xQueueReceive(s_q, &ev, to) != pdTRUE)
                    continue;
                if (ui_handle_input(&ev) == UI_INPUT_PASS &&
                    ev.type == APP_EVT_LONG_PRESS)
                    enter_portal();                           // no return
            }
        }
    }
}

void fetch_task_start(QueueHandle_t evt_q)
{
    // Audit (Backend§LOW): the task blocks on xQueueReceive(s_q, …); a NULL
    // queue would be a silent hang. Fail loudly at startup instead.
    configASSERT(evt_q);
    s_q = evt_q;
    xTaskCreate(fetch_task, "fetch", 6144, NULL, 4, NULL);
}
