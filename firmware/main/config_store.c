// firmware/main/config_store.c
#include "config_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "cfg";
static const char *NS  = "cbtoy";

static uint8_t s_brightness = CONFIG_DEFAULT_BRIGHTNESS;
static bool    s_flipped    = CONFIG_DEFAULT_DISPLAY_FLIPPED;

void config_store_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "bright", &v) == ESP_OK) s_brightness = v;
        if (nvs_get_u8(h, "flip", &v)   == ESP_OK) s_flipped = (v != 0);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "config loaded (provisioned=%d)", config_store_is_provisioned());
}

uint8_t config_store_get_brightness(void)     { return s_brightness; }
bool    config_store_get_display_flipped(void){ return s_flipped; }

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

size_t config_store_get_ssid(char *b, size_t n)        { return get_str("ssid",  b, n, NULL); }
size_t config_store_get_pass(char *b, size_t n)        { return get_str("pass",  b, n, NULL); }
size_t config_store_get_upstash_url(char *b, size_t n) { return get_str("url",   b, n, NULL); }
size_t config_store_get_upstash_key(char *b, size_t n) { return get_str("rkey",  b, n, "codexbar"); }
size_t config_store_get_upstash_token(char *b, size_t n){ return get_str("tok",  b, n, NULL); }

bool config_store_is_provisioned(void)
{
    char ssid[CFG_SSID_MAX], url[CFG_URL_MAX], tok[CFG_TOKEN_MAX];
    return config_store_get_ssid(ssid, sizeof ssid) > 0 &&
           config_store_get_upstash_url(url, sizeof url) > 0 &&
           config_store_get_upstash_token(tok, sizeof tok) > 0;
}

bool config_store_set_provisioning(const char *ssid, const char *pass,
                                   const char *url, const char *key,
                                   const char *token)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = true;
    ok &= nvs_set_str(h, "ssid", ssid ? ssid : "")   == ESP_OK;
    ok &= nvs_set_str(h, "pass", pass ? pass : "")   == ESP_OK;
    ok &= nvs_set_str(h, "url",  url  ? url  : "")   == ESP_OK;
    ok &= nvs_set_str(h, "rkey", (key && *key) ? key : "codexbar") == ESP_OK;
    ok &= nvs_set_str(h, "tok",  token ? token : "") == ESP_OK;
    if (ok) ok &= nvs_commit(h) == ESP_OK;
    nvs_close(h);
    ESP_LOGI(TAG, "provisioning %s", ok ? "saved" : "FAILED");
    return ok;
}

void config_store_clear_provisioning(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "ssid"); nvs_erase_key(h, "pass");
    nvs_erase_key(h, "url");  nvs_erase_key(h, "rkey");
    nvs_erase_key(h, "tok");
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "provisioning cleared — will reboot into captive portal");
}
