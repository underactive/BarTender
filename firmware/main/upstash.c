// firmware/main/upstash.c
#include "upstash.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "upstash";

// Accumulate the response body into the caller's buffer via the HTTP event
// callback (handles chunked transfer too).
typedef struct { char *buf; size_t cap; size_t len; bool overflow; } sink_t;

static esp_err_t on_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        sink_t *s = (sink_t *)e->user_data;
        if (!s) return ESP_OK;
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

    // Build "{url sans trailing '/'}/get/{key}".
    char full[192];
    size_t ulen = strlen(url);
    while (ulen > 0 && url[ulen - 1] == '/') ulen--;
    snprintf(full, sizeof full, "%.*s/get/%s", (int)ulen, url,
             (key && *key) ? key : "codexbar");

    char auth[300];
    snprintf(auth, sizeof auth, "Bearer %s", token);

    sink_t sink = { .buf = out, .cap = out_sz, .len = 0, .overflow = false };
    out[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = full,
        .method = HTTP_METHOD_GET,
        .event_handler = on_evt,
        .user_data = &sink,
        .crt_bundle_attach = esp_crt_bundle_attach,   // Mozilla CA bundle
        .timeout_ms = 12000,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return UPSTASH_ERR_NET;
    esp_http_client_set_header(c, "Authorization", auth);

    esp_err_t err = esp_http_client_perform(c);
    upstash_status_t rc;
    if (err != ESP_OK) {
        // esp-tls failures surface as ESP_ERR_ESP_TLS_* — treat distinctly.
        rc = (err == ESP_ERR_HTTP_CONNECT) ? UPSTASH_ERR_NET : UPSTASH_ERR_TLS;
        ESP_LOGE(TAG, "perform failed: %s", esp_err_to_name(err));
    } else {
        int status = esp_http_client_get_status_code(c);
        if (sink.overflow)               rc = UPSTASH_ERR_TOOBIG;
        else if (status == 401 || status == 403) rc = UPSTASH_ERR_AUTH;
        else if (status / 100 != 2)      rc = UPSTASH_ERR_HTTP;
        else { rc = UPSTASH_OK; if (out_len) *out_len = sink.len; }
        ESP_LOGI(TAG, "HTTP %d, %u bytes", status, (unsigned)sink.len);
    }
    esp_http_client_cleanup(c);
    return rc;
}
