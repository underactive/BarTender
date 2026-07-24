// firmware/main/upstash.c
#include "upstash.h"
#include "upstash_roots.h"
#include "config_store.h"      // CFG_*_MAX — keep request buffers in sync
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "upstash";

// Accumulate the response body into the caller's buffer via the HTTP event
// callback (handles chunked transfer too).
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
    bool tls_failed;
} sink_t;

static bool event_has_tls_failure(const esp_http_client_event_t *e)
{
    if (!e || !e->data) return false;
    const esp_tls_last_error_t *tls = (const esp_tls_last_error_t *)e->data;
    return tls->last_error >= ESP_ERR_MBEDTLS_CERT_PARTLY_OK &&
           tls->last_error <= ESP_ERR_MBEDTLS_SSL_TICKET_SETUP_FAILED;
}

static esp_err_t on_evt(esp_http_client_event_t *e)
{
    sink_t *s = (sink_t *)e->user_data;
    if (!s) return ESP_OK;

    if (e->event_id == HTTP_EVENT_ERROR) {
        // esp_http_client_perform() reduces a failed TLS handshake to the
        // generic ESP_ERR_HTTP_CONNECT. Its event still holds the esp-tls
        // reason, so retain that distinction for the user-facing status.
        s->tls_failed |= event_has_tls_failure(e);
    } else if (e->event_id == HTTP_EVENT_ON_DATA) {
        if (s->len + e->data_len >= s->cap) { s->overflow = true; return ESP_OK; }
        memcpy(s->buf + s->len, e->data, e->data_len);
        s->len += e->data_len;
        s->buf[s->len] = '\0';
    }
    return ESP_OK;
}

const char *upstash_status_str(upstash_status_t s)
{
    switch (s) {
        case UPSTASH_OK:        return "ok";
        case UPSTASH_ERR_NET:   return "network";
        case UPSTASH_ERR_TLS:   return "tls";
        case UPSTASH_ERR_AUTH:  return "auth (token?)";
        case UPSTASH_ERR_HTTP:  return "http";
        case UPSTASH_ERR_TOOBIG:return "response too big";
        default:                return "?";
    }
}

upstash_status_t upstash_get(const char *url, const char *key,
                             const char *token,
                             char *out, size_t out_sz, size_t *out_len)
{
    if (!url || !*url || !token || !*token || !out || out_sz < 2)
        return UPSTASH_ERR_NET;

    // Fix M: require https:// — an http:// URL would send the bearer token in
    // cleartext. Reject before any connection attempt.
    if (strncmp(url, "https://", 8) != 0) {
        ESP_LOGW(TAG, "upstash URL must use https:// — refusing to send token over plain HTTP");
        return UPSTASH_ERR_NET;
    }

    // Build "{url sans trailing '/'}/get/{key}". Buffers derived from
    // CFG_*_MAX so they can't silently drift if those limits change.
    // Audit Security§HIGH: check snprintf truncation — a clipped URL could
    // resolve to a different CA-valid host and replay the bearer token there.
    char full[CFG_URL_MAX + 8 + CFG_KEY_MAX];
    size_t ulen = strlen(url);
    while (ulen > 0 && url[ulen - 1] == '/') ulen--;
    int fn = snprintf(full, sizeof full, "%.*s/get/%s", (int)ulen, url,
                      (key && *key) ? key : "codexbar");
    if (fn < 0 || fn >= (int)sizeof full) return UPSTASH_ERR_NET;

    char auth[CFG_TOKEN_MAX + 8];
    int an = snprintf(auth, sizeof auth, "Bearer %s", token);
    if (an < 0 || an >= (int)sizeof auth) return UPSTASH_ERR_AUTH;

    sink_t sink = {
        .buf = out,
        .cap = out_sz,
        .len = 0,
        .overflow = false,
        .tls_failed = false,
    };
    out[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = full,
        .method = HTTP_METHOD_GET,
        .event_handler = on_evt,
        .user_data = &sink,
        // ESP-IDF v5.3.2's compact CA-bundle callback rejects Upstash's
        // valid 2026 Let's Encrypt hierarchy. Supplying the trusted root here
        // uses mbedTLS's full chain validator — hostname/cert checks remain
        // enforced; this is deliberately not a no-verify workaround.
        .cert_pem = s_upstash_trusted_root_pem,
        .timeout_ms = 12000,
        // Audit Security§HIGH: Upstash /get never legitimately 30x's; with
        // redirects on, a poisoned hop could replay the bearer token to any
        // CA-valid host. Disable cross-host credential forwarding.
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return UPSTASH_ERR_NET;
    esp_http_client_set_header(c, "Authorization", auth);

    esp_err_t err = esp_http_client_perform(c);
    upstash_status_t rc;
    if (err != ESP_OK) {
        // HTTP client collapses an SSL handshake failure to HTTP_CONNECT;
        // on_evt() preserved the underlying esp-tls signal. DNS/connect/
        // timeout failures still remain network errors.
        bool is_tls = sink.tls_failed ||
                      (err >= ESP_ERR_ESP_TLS_BASE &&
                       err <  ESP_ERR_ESP_TLS_BASE + 0x100);
        rc = is_tls ? UPSTASH_ERR_TLS : UPSTASH_ERR_NET;
        ESP_LOGE(TAG, "perform failed: %s", esp_err_to_name(err));
    } else {
        int status = esp_http_client_get_status_code(c);
        if (sink.overflow)               rc = UPSTASH_ERR_TOOBIG;
        else if (status == 401 || status == 403) rc = UPSTASH_ERR_AUTH;
        else if (status / 100 != 2)      rc = UPSTASH_ERR_HTTP;
        else { rc = UPSTASH_OK; if (out_len) *out_len = sink.len; }
        ESP_LOGI(TAG, "HTTP %d, %u bytes", status, (unsigned)sink.len);
        // Audit (Backend§LOW): on a non-2xx, surface a bounded snippet of the
        // error body so a 4xx/5xx from the store is debuggable from logs alone.
        // The bearer token rides in the request header, never the response
        // body, so this cannot leak the credential. sink.buf is NUL-terminated.
        if (status / 100 != 2 && sink.len > 0)
            ESP_LOGW(TAG, "HTTP %d error body: %.120s", status, sink.buf);
    }
    esp_http_client_cleanup(c);
    return rc;
}
