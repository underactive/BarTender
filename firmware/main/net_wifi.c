// firmware/main/net_wifi.c
//
// Multi-network WiFi STA. A single `wifi_mgr` task owns the whole connect
// lifecycle: when down it scans, intersects the air with the remembered list,
// connects to the strongest in-range network (with stickiness), and on success
// promotes that SSID to MRU. The WiFi/IP event handler is a PURE SIGNALLER —
// it only sets two single-word flags and pokes the mgr task. This keeps the
// documented lock-free `s_connected` invariant intact and keeps every NVS
// write + scan off the event task (an NVS/scan stall there blocks the stack).
#include "net_wifi.h"
#include "config_store.h"
#include "ui.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "wifi";

#define SCAN_MAX_AP        24
#define CONNECT_WAIT_MS    12000   // per-attempt: wait for GOT_IP / DISCONNECT
#define STICKY_DB          8       // don't switch APs unless >= this much better
#define SWEEPS_NO_KNOWN_MIN 2      // net_wifi_no_known_network() threshold; the
                                   // fetch.c self-heal gate depends on this
#define SWEEPS_SAT         1000000 // s_sweeps_no_cand saturation: only the
                                   // >= MIN test matters; cap prevents int wrap
                                   // over months of uptime with no known AP
// Escalating delay between failed scan sweeps; saturates at 60 s so a relocated
// toy still rescans well inside the 60 s (FETCH_INTERVAL_S) poll window.
static const int s_backoff_s[] = { 5, 15, 60 };

// WHY (unchanged invariant): single word, sole writer = WiFi event task, read
// by the mgr + fetch tasks. No tearing on Xtensa → intentionally lock-free,
// do NOT add a mutex. Audit State§LOW confirmed this is a legitimate case.
static volatile bool s_connected = false;
// Same discipline, new var: sole writer = event task (one store per
// disconnect), sole reader = mgr task. Single word, lock-free by the same
// argument as s_connected.
static volatile int  s_last_disc_reason = 0;
// Sole writer = mgr task, sole reader = fetch task (net_wifi_no_known_network).
static volatile int  s_sweeps_no_cand = 0;

// Published by xTaskCreate() in net_wifi_start_multi() BEFORE esp_wifi_start()
// (task creation is a full memory barrier; the handle is therefore visible to
// the event task before STA_START can fire, so the wake is never dropped —
// Audit Resource&Concurrency§P1-3). on_wifi() still NULL-guards defensively.
static TaskHandle_t  s_mgr;

// mgr-task-local state (only the mgr task touches these — never the event task)
static wifi_creds_t      s_list;                       // ~490 B, off-stack
static wifi_ap_record_t  s_recs[SCAN_MAX_AP];          // ~2 KB, off-stack
static char              s_winner[CFG_SSID_MAX];       // SSID of the live attempt
static char              s_last_ok[CFG_SSID_MAX];      // last associated (stickiness)
static char              s_skip[CFG_WIFI_MAX_ENTRIES][CFG_SSID_MAX];
static int               s_skip_n;

// ---- helpers ---------------------------------------------------------------

// Copy printable ASCII only (the LCD font has no glyphs outside 0x20-0x7E);
// non-ASCII SSID bytes → '?'. Truncates to fit. Keeps status text legible.
static void san(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; in && *in && o + 1 < n; in++)
        out[o++] = (*in >= 0x20 && *in <= 0x7E) ? *in : '?';
    out[o] = '\0';
}

static void status_ssid(const char *fmt, const char *ssid)
{
    char clean[24], line[64];
    san(ssid, clean, sizeof clean);
    snprintf(line, sizeof line, fmt, clean);
    ui_set_status(line);
}

static bool reason_is_auth(int r)
{
    switch (r) {
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_NOT_AUTHED:
        case WIFI_REASON_MIC_FAILURE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return true;                  // wrong PSK / handshake — skip this SSID
        default:
            return false;                 // NO_AP_FOUND etc. → just out of range
    }
}

static bool skipped(const char *ssid)
{
    for (int i = 0; i < s_skip_n; i++)
        if (strncmp(s_skip[i], ssid, CFG_SSID_MAX) == 0) return true;
    return false;
}

static void skip_add(const char *ssid)
{
    if (s_skip_n < CFG_WIFI_MAX_ENTRIES)
        strlcpy(s_skip[s_skip_n++], ssid, CFG_SSID_MAX);
}

static void list_reload(void)
{
    memset(&s_list, 0, sizeof s_list);
    uint8_t c = config_store_wifi_count();
    for (uint8_t i = 0; i < c && i < CFG_WIFI_MAX_ENTRIES; i++)
        config_store_wifi_get(i, s_list.e[i].ssid, CFG_SSID_MAX,
                                 s_list.e[i].pass, CFG_PASS_MAX);
    s_list.count = c;
}

// Strongest remembered + in-range SSID, honoring the per-sweep skip-set and an
// 8 dB stickiness margin toward the last-associated SSID (prevents ping-pong
// between two near-equal APs across reconnects). Returns the s_list index or -1.
static int select_candidate(int n)
{
    int best = -1, best_rssi = -128, sticky = -1, sticky_rssi = -128;
    for (uint8_t i = 0; i < s_list.count; i++) {
        const char *ss = s_list.e[i].ssid;
        if (!ss[0] || skipped(ss)) continue;
        int r = -128, seen = 0;
        for (int k = 0; k < n; k++)
            if (strncmp((char *)s_recs[k].ssid, ss, CFG_SSID_MAX) == 0) {
                if (!seen || s_recs[k].rssi > r) r = s_recs[k].rssi;
                seen = 1;
            }
        if (!seen) continue;
        if (r > best_rssi || best < 0) { best_rssi = r; best = i; }   // MRU tie: i ascending
        if (s_last_ok[0] && strncmp(ss, s_last_ok, CFG_SSID_MAX) == 0) {
            sticky = i; sticky_rssi = r;
        }
    }
    if (best < 0) return -1;
    if (sticky >= 0 && sticky_rssi >= best_rssi - STICKY_DB) return sticky;
    return best;
}

// Configure + start one association attempt; return false if no usable
// candidate (caller handles the no-candidate path).
static bool try_connect(int idx)
{
    strlcpy(s_winner, s_list.e[idx].ssid, sizeof s_winner);
    wifi_config_t wc = { 0 };
    // Known limitation (Audit QA§P1-3, accepted): wc.sta.ssid is uint8_t[32];
    // strlcpy NUL-terminates, so a full 32-octet SSID round-trips as 31 chars
    // and won't associate. Practical SSID limit here is 31. Rare in the wild;
    // tracked in the exec-plan follow-ups rather than reworking string storage.
    strlcpy((char *)wc.sta.ssid,     s_list.e[idx].ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, s_list.e[idx].pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;   // WPA2-PSK only, by design
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) return false;
    status_ssid("WiFi: connecting %s", s_winner);
    // Audit Resource&Concurrency§P1-1: FreeRTOS notifications are binary-
    // latched, so a leftover give from the previous attempt's DISCONNECTED
    // (or the 3 s STA_START fallback) would make the NEXT await_outcome()
    // return instantly with a STALE s_last_disc_reason. Clear the pending
    // notification so await_outcome only ever reacts to THIS attempt's
    // GOT_IP / DISCONNECTED. (mgr task is the only waiter — safe to clear.)
    xTaskNotifyStateClear(NULL);
    return esp_wifi_connect() == ESP_OK;
}

// Block until the event task reports THIS attempt's outcome (or it stalls).
// Returns true iff associated + got IP. Correct because try_connect() cleared
// any stale notification first, so every wake here is caused by the in-flight
// attempt: GOT_IP (s_connected=true → true) or DISCONNECTED (false → false).
static bool await_outcome(void)
{
    int64_t end = esp_timer_get_time() + (int64_t)CONNECT_WAIT_MS * 1000;
    while (!s_connected) {
        int64_t rem = (end - esp_timer_get_time()) / 1000;
        if (rem <= 0) return false;                 // stalled — treat as failed
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(rem > 500 ? 500 : (int)rem)))
            if (!s_connected) return false;         // DISCONNECTED woke us
    }
    return true;
}

// ---- event handler: PURE SIGNALLER ----------------------------------------

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // Deliberately NOT esp_wifi_connect() — the mgr task drives connect
        // only after a scan. Just wake it so it can start scanning.
        if (s_mgr) xTaskNotifyGive(s_mgr);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_last_disc_reason = d ? d->reason : 0;
        s_connected = false;
        if (s_mgr) xTaskNotifyGive(s_mgr);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        if (s_mgr) xTaskNotifyGive(s_mgr);
        ESP_LOGI(TAG, "connected, got IP");
    }
}

// ---- manager task ----------------------------------------------------------

static void wifi_mgr_task(void *arg)
{
    (void)arg;
    // Wait for the STA to come up before the first scan (STA_START notifies
    // us; the timeout is a belt-and-braces fallback if it was missed).
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));

    int backoff = 0;
    for (;;) {
        if (s_connected) {                 // idle until the link actually drops
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        // ---- fresh sweep ----
        s_skip_n = 0;
        list_reload();
        ui_set_status("WiFi: scanning...");
        wifi_scan_config_t sc = { 0 };
        sc.show_hidden = true;
        sc.scan_type   = WIFI_SCAN_TYPE_ACTIVE;
        sc.scan_time.active.min = 100;
        sc.scan_time.active.max = 150;     // ~14 ch * 150ms => well under 4 s
        if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        // Audit QA§P1-2: on failure esp_wifi_scan_get_ap_records leaves `n`
        // undefined — using it would scan stale/garbage s_recs. Bail the sweep.
        uint16_t total = 0;
        esp_wifi_scan_get_ap_num(&total);             // true count (pre-read)
        uint16_t n = SCAN_MAX_AP;
        if (esp_wifi_scan_get_ap_records(&n, s_recs) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (n > SCAN_MAX_AP) n = SCAN_MAX_AP;
        // Diagnostic: total >> n ⇒ the wanted AP may be truncated out of the
        // SCAN_MAX_AP buffer (RF-dense); total small + SSID absent ⇒ band/
        // range/SSID-string. Either way the direct-MRU fallback below rescues.
        ESP_LOGW(TAG, "scan: %u APs total, %u read; MRU='%s'",
                 total, n, s_list.count ? s_list.e[0].ssid : "-");

        // Try candidates strongest-first; auth failures skip that SSID for
        // the rest of THIS sweep so a wrong saved password can't infinite-loop.
        bool associated = false, saw_candidate = false;
        for (;;) {
            int idx = select_candidate(n);
            if (idx < 0) break;                       // none left this sweep
            saw_candidate = true;                     // a remembered AP IS here
            if (!try_connect(idx)) { skip_add(s_list.e[idx].ssid); continue; }
            if (await_outcome()) { associated = true; break; }
            if (reason_is_auth(s_last_disc_reason))
                status_ssid("WiFi: wrong password %s", s_winner);
            skip_add(s_winner);                       // wrong pass / not here now
        }

        // Direct-connect fallback (regression fix): the OLD firmware connected
        // with NO app-level scan and worked on this network/spot. The new
        // scan→strncmp gate can miss a present AP — an RF-dense site with
        // more APs than SCAN_MAX_AP (esp_wifi_scan_get_ap_records truncates,
        // unordered), a hidden SSID, or a too-short active dwell. When the
        // scan matched nothing, still try the MRU network directly: esp_wifi
        // runs its own targeted join (exactly the proven old path). One
        // attempt per sweep; respects the auth skip-set.
        if (!associated && !saw_candidate && s_list.count > 0 &&
            s_list.e[0].ssid[0] && !skipped(s_list.e[0].ssid)) {
            ESP_LOGW(TAG, "scan blind — direct-connect MRU '%s'",
                     s_list.e[0].ssid);
            if (try_connect(0) && await_outcome()) {
                associated = true;
            } else {
                if (reason_is_auth(s_last_disc_reason))
                    status_ssid("WiFi: wrong password %s", s_list.e[0].ssid);
                skip_add(s_list.e[0].ssid);
            }
        }

        if (associated) {
            s_sweeps_no_cand = 0;
            backoff = 0;
            config_store_wifi_promote(s_winner);      // MRU bump (mgr task)
            strlcpy(s_last_ok, s_winner, sizeof s_last_ok);
            ESP_LOGI(TAG, "associated + promoted '%s'", s_winner);
            continue;                                  // loop → idle while up
        }

        // Audit QA§P1-2: only count a sweep as "no known network" when ZERO
        // remembered SSIDs were even in range. If one was present but
        // unjoinable (wrong saved password / transient), the self-heal gate
        // must stay CLOSED — relocating is different from "can't auth here",
        // and a ~minutes router outage on a fresh boot must not pop the portal.
        if (saw_candidate) {
            s_sweeps_no_cand = 0;
        } else if (s_sweeps_no_cand < SWEEPS_SAT) {
            s_sweeps_no_cand++;
        }
        ui_set_status(saw_candidate ? "WiFi: cannot join saved network"
                                    : "WiFi: no known network");
        int s = s_backoff_s[backoff];
        if (backoff < (int)(sizeof s_backoff_s / sizeof s_backoff_s[0]) - 1)
            backoff++;
        ESP_LOGW(TAG, "sweep failed (saw_candidate=%d) — rescan in %ds (no_cand=%d)",
                 saw_candidate, s, s_sweeps_no_cand);
        vTaskDelay(pdMS_TO_TICKS((uint32_t)s * 1000));
    }
}

// ---- public API ------------------------------------------------------------

bool net_wifi_is_connected(void)     { return s_connected; }
bool net_wifi_no_known_network(void) { return s_sweeps_no_cand >= SWEEPS_NO_KNOWN_MIN; }

void net_wifi_start_multi(void)
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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Create the mgr task (publishing s_mgr) BEFORE esp_wifi_start() so the
    // STA_START wake can't be lost. The mgr task replaces the old esp_timer
    // retry: a single blocking task serializes scan→select→connect→backoff,
    // so the flap-guard race the timer needed (Audit QA§LOW on the old
    // esp_timer rearm) cannot occur here.
    // 6144 (not 4096): the mgr task also calls the blob config_store_wifi_*
    // functions (each puts a ~490 B wifi_creds_t on the stack) plus the deep
    // NVS/flash path on promote — same overflow class that crashed the httpd
    // task. Headroom over s_recs handling + nvs depth.
    xTaskCreate(wifi_mgr_task, "wifi_mgr", 6144, NULL, 4, &s_mgr);
    ESP_ERROR_CHECK(esp_wifi_start());     // STA_START → on_wifi wakes the mgr
    ESP_LOGI(TAG, "multi-WiFi manager started (%u remembered)",
             config_store_wifi_count());
}
