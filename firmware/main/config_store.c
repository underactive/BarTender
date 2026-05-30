// firmware/main/config_store.c
#include "config_store.h"
#include "nvs.h"            // nvs_flash_init lives in main.c; we only need nvs.h
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>

static const char *TAG = "cfg";
static const char *NS  = "cbtoy";

static uint8_t s_brightness = CONFIG_DEFAULT_BRIGHTNESS;
static bool    s_flipped    = CONFIG_DEFAULT_DISPLAY_FLIPPED;

// Read a string key into buf (always NUL-terminated). Returns strlen.
static size_t get_str(const char *key, char *buf, size_t n, const char *dflt)
{
    if (!buf || n == 0) return 0;
    buf[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = n;
        esp_err_t e = nvs_get_str(h, key, buf, &len);
        nvs_close(h);
        if (e == ESP_OK && buf[0] != '\0') return strnlen(buf, n);
    }
    if (dflt) { strlcpy(buf, dflt, n); return strnlen(buf, n); }
    buf[0] = '\0';
    return 0;
}

// ---- remembered-WiFi blob ("wnets") ---------------------------------------

// Struct version written to new blobs. Old blobs have struct_version == 0
// (zero-initialised padding). Accept 0 (baseline) and WIFI_BLOB_VERSION (current).
// Any higher version means a newer firmware wrote it — reject to avoid misparse.
#define WIFI_BLOB_VERSION  1u

static void blob_init_empty(wifi_creds_t *w)
{
    memset(w, 0, sizeof *w);
    w->magic          = CFG_WIFI_BLOB_MAGIC;
    w->struct_version = WIFI_BLOB_VERSION;
    w->count          = 0;
}

// Load + VALIDATE the blob. Any inconsistency (missing, wrong size, bad magic,
// impossible count, malformed entry, unknown version) => return false. The
// caller treats false as "zero remembered networks"; we deliberately never
// erase a bad blob (a future firmware might recover it) and never abort/brick.
static bool blob_load(wifi_creds_t *w)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof *w;
    esp_err_t e = nvs_get_blob(h, "wnets", w, &len);
    nvs_close(h);
    if (e != ESP_OK || len != sizeof *w) return false;
    if (w->magic != CFG_WIFI_BLOB_MAGIC)  return false;
    // Accept version 0 (old blobs with zero-initialised padding) and the
    // current version. Reject anything higher for forward compatibility.
    if (w->struct_version > WIFI_BLOB_VERSION) return false;
    if (w->count > CFG_WIFI_MAX_ENTRIES)  return false;
    for (uint8_t i = 0; i < w->count; i++) {
        size_t sl = strnlen(w->e[i].ssid, CFG_SSID_MAX);
        if (sl == 0 || sl >= CFG_SSID_MAX) return false;
        if (strnlen(w->e[i].pass, CFG_PASS_MAX) >= CFG_PASS_MAX) return false;
    }
    return true;
}

// One nvs_set_blob + nvs_commit => an LRU rotate/evict is atomic (a torn write
// leaves the prior valid blob, never a half record).
static bool blob_save(const wifi_creds_t *w)
{
    // Stamp the current version before writing so newly saved blobs are always
    // recognised as version WIFI_BLOB_VERSION on read-back.
    wifi_creds_t tmp = *w;
    tmp.struct_version = WIFI_BLOB_VERSION;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_blob(h, "wnets", &tmp, sizeof tmp);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

// Idempotent legacy fold. If no valid blob exists but the old single-SSID
// keys do, seed wnets[0] from them. Blob presence is the "migration done"
// marker, so this never re-folds and never needs versioning. Legacy keys are
// intentionally NOT erased (old-firmware rollback stays possible) and Upstash
// keys are never touched. No portal is forced.
static void migrate_legacy_wifi(void)
{
    wifi_creds_t w;
    if (blob_load(&w)) return;                       // valid blob already

    char ssid[CFG_SSID_MAX], pass[CFG_PASS_MAX];
    if (get_str("ssid", ssid, sizeof ssid, NULL) > 0) {
        get_str("pass", pass, sizeof pass, NULL);
        blob_init_empty(&w);
        strlcpy(w.e[0].ssid, ssid, sizeof w.e[0].ssid);
        strlcpy(w.e[0].pass, pass, sizeof w.e[0].pass);
        w.count = 1;
        if (blob_save(&w))
            ESP_LOGW(TAG, "migrated legacy SSID '%s' -> wnets[0]", ssid);
    } else {
        // Fresh chip / unprovisioned: write the empty blob so later reads hit
        // the fast valid-blob path instead of re-running this every boot.
        blob_init_empty(&w);
        blob_save(&w);
    }
}

void config_store_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "bright", &v) == ESP_OK) s_brightness = v;
        if (nvs_get_u8(h, "flip", &v)   == ESP_OK) s_flipped = (v != 0);
        nvs_close(h);
    }
    migrate_legacy_wifi();
    ESP_LOGI(TAG, "config loaded (upstash=%d wifi=%u)",
             config_store_has_upstash(), config_store_wifi_count());
}

uint8_t config_store_get_brightness(void)     { return s_brightness; }
bool    config_store_get_display_flipped(void){ return s_flipped; }

// ---- Upstash credentials (independent of WiFi) ----------------------------

size_t config_store_get_upstash_url(char *b, size_t n)  { return get_str("url",  b, n, NULL); }
size_t config_store_get_upstash_key(char *b, size_t n)  { return get_str("rkey", b, n, "codexbar"); }
size_t config_store_get_upstash_token(char *b, size_t n){ return get_str("tok",  b, n, NULL); }

bool config_store_has_upstash(void)
{
    char url[CFG_URL_MAX], tok[CFG_TOKEN_MAX];
    return config_store_get_upstash_url(url, sizeof url) > 0 &&
           config_store_get_upstash_token(tok, sizeof tok) > 0;
}

bool config_store_set_upstash(const char *url, const char *key, const char *token)
{
    // Fix F: reject values that would silently truncate on read-back.
    if (url   && strnlen(url,   CFG_URL_MAX)   >= CFG_URL_MAX)   { ESP_LOGE(TAG, "url too long");   return false; }
    if (key   && strnlen(key,   CFG_KEY_MAX)   >= CFG_KEY_MAX)   { ESP_LOGE(TAG, "key too long");   return false; }
    if (token && strnlen(token, CFG_TOKEN_MAX) >= CFG_TOKEN_MAX) { ESP_LOGE(TAG, "token too long"); return false; }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = true;
    ok &= nvs_set_str(h, "url",  url   ? url   : "") == ESP_OK;
    ok &= nvs_set_str(h, "rkey", (key && *key) ? key : "codexbar") == ESP_OK;
    ok &= nvs_set_str(h, "tok",  token ? token : "") == ESP_OK;
    if (ok) ok &= nvs_commit(h) == ESP_OK;
    nvs_close(h);
    ESP_LOGI(TAG, "upstash %s", ok ? "saved" : "FAILED");
    return ok;
}

// ---- Remembered WiFi networks (MRU-ordered, LRU-evicted) ------------------

uint8_t config_store_wifi_count(void)
{
    wifi_creds_t w;
    return blob_load(&w) ? w.count : 0;
}

size_t config_store_wifi_get(uint8_t i, char *ssid, size_t sn,
                                         char *pass, size_t pn)
{
    if (ssid && sn) ssid[0] = '\0';
    if (pass && pn) pass[0] = '\0';
    wifi_creds_t w;
    if (!blob_load(&w) || i >= w.count) return 0;
    if (ssid && sn) strlcpy(ssid, w.e[i].ssid, sn);
    if (pass && pn) strlcpy(pass, w.e[i].pass, pn);
    return strnlen(w.e[i].ssid, CFG_SSID_MAX);
}

// Find an exact SSID match in e[0..count-1]; -1 if absent.
static int wifi_find(const wifi_creds_t *w, const char *ssid)
{
    for (uint8_t i = 0; i < w->count; i++)
        if (strncmp(w->e[i].ssid, ssid, CFG_SSID_MAX) == 0) return (int)i;
    return -1;
}

bool config_store_wifi_add_or_update(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return false;
    if (strnlen(ssid, CFG_SSID_MAX) >= CFG_SSID_MAX) return false;       // too long
    if (pass && strnlen(pass, CFG_PASS_MAX) >= CFG_PASS_MAX) return false;

    wifi_creds_t w;
    if (!blob_load(&w)) blob_init_empty(&w);

    wifi_entry_t ne;
    memset(&ne, 0, sizeof ne);
    strlcpy(ne.ssid, ssid, sizeof ne.ssid);
    strlcpy(ne.pass, pass ? pass : "", sizeof ne.pass);

    int j = wifi_find(&w, ssid);
    if (j >= 0) {
        // Promote the existing slot to MRU, replacing its password in place.
        for (int k = j; k > 0; k--) w.e[k] = w.e[k - 1];
        w.e[0] = ne;                                 // count unchanged
    } else {
        // Prepend; if full, the shift drops e[count-1] (the LRU entry).
        uint8_t keep = w.count;
        if (keep == CFG_WIFI_MAX_ENTRIES) keep = CFG_WIFI_MAX_ENTRIES - 1;
        for (int k = keep; k > 0; k--) w.e[k] = w.e[k - 1];
        w.e[0] = ne;
        if (w.count < CFG_WIFI_MAX_ENTRIES) w.count++;
    }
    for (uint8_t i = w.count; i < CFG_WIFI_MAX_ENTRIES; i++)
        memset(&w.e[i], 0, sizeof w.e[i]);           // no stale pass residue
    w.magic = CFG_WIFI_BLOB_MAGIC;

    bool ok = blob_save(&w);
    ESP_LOGI(TAG, "wifi add/update '%s' -> count=%u %s",
             ssid, w.count, ok ? "ok" : "FAIL");
    return ok;
}

bool config_store_wifi_promote(const char *ssid)
{
    if (!ssid || !ssid[0]) return false;
    wifi_creds_t w;
    if (!blob_load(&w)) return false;
    int j = wifi_find(&w, ssid);
    if (j < 0)  return false;
    if (j == 0) return true;                         // already MRU; skip commit
    wifi_entry_t e = w.e[j];
    for (int k = j; k > 0; k--) w.e[k] = w.e[k - 1];
    w.e[0] = e;
    bool ok = blob_save(&w);
    ESP_LOGI(TAG, "wifi promote '%s' (was idx %d) %s", ssid, j, ok ? "ok" : "FAIL");
    return ok;
}

// (config_store_wifi_clear_all removed — confirmed zero callers in firmware tree.)

// ---- SoftAP PSK (random, generated once at first boot) --------------------

// Alphanumeric charset without ambiguous chars (0/O, 1/I/l).
// 34 chars -> ~5.09 bits/char; 12 chars -> ~61 bits entropy (well above WPA2 needs).
static const char AP_PSK_CHARS[] =
    "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz";
#define AP_PSK_LEN  12   // chars; must be >= 8 for WPA2

bool config_store_get_or_create_ap_psk(char *out, size_t n)
{
    if (!out || n < AP_PSK_LEN + 1) return false;
    // Try to load an existing PSK first.
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = n;
        esp_err_t e = nvs_get_str(h, "ap_psk", out, &len);
        nvs_close(h);
        if (e == ESP_OK && strnlen(out, n) >= 8) return true;
    }
    // None found — generate a fresh random PSK and persist it.
    const size_t charset_len = sizeof(AP_PSK_CHARS) - 1;  // exclude NUL
    for (int i = 0; i < AP_PSK_LEN; i++) {
        // Use esp_random() (true RNG); mask to avoid modulo bias.
        uint32_t r;
        do { r = esp_random() & 0xFF; } while (r >= (256u / charset_len) * charset_len);
        out[i] = AP_PSK_CHARS[r % charset_len];
    }
    out[AP_PSK_LEN] = '\0';
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_str(h, "ap_psk", out) == ESP_OK) nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "generated new AP PSK (stored in NVS)");
    }
    return true;
}

// ---- one-shot "open portal to ADD a network" flag -------------------------

void config_store_request_portal(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, "fprov", 1) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

bool config_store_take_portal_request(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t v = 0;
    bool set = (nvs_get_u8(h, "fprov", &v) == ESP_OK && v != 0);
    if (set) {                                       // clear-BEFORE-act:
        nvs_erase_key(h, "fprov");                    // consumed exactly once,
        nvs_commit(h);                                // so power loss in the
    }                                                 // portal can't boot-loop
    nvs_close(h);
    return set;
}

// (Legacy compatibility functions removed — confirmed zero callers in firmware tree.)
