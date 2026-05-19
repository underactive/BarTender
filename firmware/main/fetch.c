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

// Triple-tap detector state. File-scope (not function-local as before) so the
// SAME detector is fed by both the connect-wait and the steady-state loop —
// the re-provision gesture must survive a boot that never associates.
// Single producer/consumer (the one fetch task), like net_wifi.c's s_connected.
static int64_t s_taps[3];
static int     s_tc;

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
    // Audit QA§P2-3: a triple-tap inside the 900 ms pre-restart window would
    // re-enter this (note_tap calls it again); the guard makes it idempotent
    // so the request/delay/restart isn't stacked. fetch_task is the only
    // caller, so a plain static is sufficient (no concurrency here).
    static bool s_porting;
    if (s_porting) return;
    s_porting = true;
    ESP_LOGW(TAG, "opening setup portal (all creds kept)");
    ui_set_status("opening setup... rebooting");
    config_store_request_portal();
    vTaskDelay(pdMS_TO_TICKS(900));
    esp_restart();
}

// Feed one accepted TAP into the triple-tap detector. On a deliberate 3-tap
// burst — all within 1200 ms AND each consecutive gap <= 600 ms, so stray
// FT6336 noise over weeks of uptime can't trigger it (touch.c debounces at
// 200 ms, so an intentional triple-tap comfortably fits) — it opens the
// add-network portal (NEVER returns). Audit QA§MED timing preserved verbatim;
// only the action changed (non-destructive) and the call site widened.
static void note_tap(void)
{
    int64_t t = now_ms();
    s_taps[0] = s_taps[1]; s_taps[1] = s_taps[2]; s_taps[2] = t;
    if (s_tc < 3) s_tc++;
    if (s_tc == 3 &&
        (t - s_taps[0]) <= 1200 &&
        (s_taps[1] - s_taps[0]) <= 600 &&
        (s_taps[2] - s_taps[1]) <= 600) {
        enter_portal();                               // no return
    }
}

// Block until the first association, BUT keep servicing the event queue so the
// triple-tap add-network gesture works even when no remembered SSID is in
// range. Audit Reliability§HIGH: the OLD code spun in a bare `while(!connected)
// vTaskDelay` here, so the queue was never drained — taps piled up unread and
// the ONLY on-device recovery was unreachable behind the very failure it
// recovers from. Self-heal: a relocated toy nobody is there to tap auto-opens
// the add-network portal — but ONLY once net_wifi has confirmed (>= 2 scan
// sweeps) that ZERO remembered networks are in range, AND the grace period
// elapsed. That gate stops a slow multi-network sweep or a wrong-password
// auth-retry loop from spuriously popping the portal. It is now NON-
// destructive (config_store_request_portal keeps all WiFi + Upstash), and
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
        if (ui_handle_input(&ev) == UI_INPUT_CONSUMED)
            continue;                                 // menu/swipe owns it first
        if (ev.type == APP_EVT_TAP)
            note_tap();                               // triple-tap → no return
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

    for (;;) {
        if (refresh_now) {
            bool ok = net_wifi_is_connected() ? do_fetch(url, key, tok)
                                              : (ui_set_status("WiFi: reconnecting..."), false);
            refresh_now = false;
            int wait_s = ok ? FETCH_INTERVAL_S : FETCH_RETRY_S;
            int64_t deadline = now_ms() + (int64_t)wait_s * 1000;

            // Wait for the deadline, but wake early on a tap.
            while (!refresh_now) {
                int64_t rem = deadline - now_ms();
                if (rem <= 0) { refresh_now = true; break; }
                app_evt_t ev;
                TickType_t to = pdMS_TO_TICKS(rem > 1000 ? 1000 : (int)rem);
                if (xQueueReceive(s_q, &ev, to) != pdTRUE)
                    continue;
                // Nav owns the event first. CONSUMED => menu/card/swipe handled
                // it: no refresh, and (critically) it never reaches the
                // triple-tap counter, so menu taps can't factory-reset.
                if (ui_handle_input(&ev) == UI_INPUT_CONSUMED)
                    continue;
                // PASS: summary screen. Only a TAP gets here.
                if (ev.type == APP_EVT_TAP) {
                    note_tap();                               // triple-tap → no return
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
