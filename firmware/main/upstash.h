// firmware/main/upstash.h
//
// One HTTPS GET against the Upstash REST API:
//   GET {url}/get/{key}   Authorization: Bearer <read-only token>
// Returns the raw response body, e.g. {"result":"<escaped-json>"} — the
// caller (stats_model) does the two-step parse. TLS trust via the bundled
// Mozilla CA store (esp_crt_bundle); no cert pinning, survives rotation.
#pragma once

#include <stddef.h>

typedef enum {
    UPSTASH_OK = 0,
    UPSTASH_ERR_NET,     // DNS/connect/transport
    UPSTASH_ERR_TLS,     // certificate / handshake
    UPSTASH_ERR_AUTH,    // HTTP 401/403 — bad/expired token
    UPSTASH_ERR_HTTP,    // other non-2xx
    UPSTASH_ERR_TOOBIG,  // body exceeded caller buffer
} upstash_status_t;

// Synchronous; call from the fetch task (never the UI task). `out` receives
// the NUL-terminated body; *out_len set to its length on UPSTASH_OK.
upstash_status_t upstash_get(const char *url, const char *key,
                             const char *token,
                             char *out, size_t out_sz, size_t *out_len);

const char *upstash_status_str(upstash_status_t s);
