// firmware/main/config_store.h
//
// NVS-backed config (pattern adapted from clawd-tank's config_store). Holds:
//   - display brightness + flip (required by the vendored display.c, unchanged)
//   - WiFi STA credentials + Upstash URL/key/READ-ONLY token (set via the
//     captive-portal provisioning flow, never compiled into the binary)
//
// All strings are NUL-terminated. Getters copy into a caller buffer and
// return the length (0 if unset). NVS namespace: "cbtoy".
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define CONFIG_DEFAULT_BRIGHTNESS       230  // 90% of 8-bit PWM duty
#define CONFIG_DEFAULT_DISPLAY_FLIPPED  0

// Max field sizes (NVS string values; generous, kept small for the form).
#define CFG_SSID_MAX   33   // 802.11 SSID <= 32 + NUL
#define CFG_PASS_MAX   64
#define CFG_URL_MAX    128  // https://<db>.upstash.io
#define CFG_KEY_MAX    48   // redis key, default "codexbar"
#define CFG_TOKEN_MAX  256  // Upstash read-only bearer token

// Load NVS into memory (or defaults). Call once, before display_init().
void config_store_init(void);

// --- Required by the vendored display.c (do not remove) ---
uint8_t config_store_get_brightness(void);
bool    config_store_get_display_flipped(void);

// --- Provisioned credentials ---
// true once a WiFi SSID and an Upstash URL+token are all present.
bool config_store_is_provisioned(void);

// Copy a field into buf (always NUL-terminated); returns strlen written.
size_t config_store_get_ssid(char *buf, size_t n);
size_t config_store_get_pass(char *buf, size_t n);
size_t config_store_get_upstash_url(char *buf, size_t n);
size_t config_store_get_upstash_key(char *buf, size_t n);   // defaults to "codexbar"
size_t config_store_get_upstash_token(char *buf, size_t n);

// Persist all provisioning fields atomically (single NVS commit). Returns true
// on success. Empty key is allowed (defaults applied on read).
bool config_store_set_provisioning(const char *ssid, const char *pass,
                                   const char *url, const char *key,
                                   const char *token);

// Wipe all credential keys (triple-tap re-provision). Brightness/flip kept.
void config_store_clear_provisioning(void);
