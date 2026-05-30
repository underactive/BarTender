// firmware/main/config_store.h
//
// NVS-backed config (pattern adapted from clawd-tank's config_store). Holds:
//   - display brightness + flip (required by the vendored display.c, unchanged)
//   - up to CFG_WIFI_MAX_ENTRIES remembered WiFi networks, MRU-ordered, in a
//     single versioned blob ("wnets"); LRU-evicted when a 6th is added
//   - Upstash URL/key/READ-ONLY token as independent string keys, decoupled
//     from WiFi so changing networks never requires re-entering them
//   - a one-shot "fprov" flag that asks the next boot to open the captive
//     portal to ADD a network (non-destructive triple-tap / self-heal)
//
// All set via the captive portal, never compiled into the binary. All strings
// are NUL-terminated. Getters copy into a caller buffer and return the length
// (0 if unset). NVS namespace: "cbtoy".
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define CONFIG_DEFAULT_BRIGHTNESS       230  // 90% of 8-bit PWM duty
#define CONFIG_DEFAULT_DISPLAY_FLIPPED  0   // native orientation; panel mounted USB-down

// Max field sizes (NVS string values; generous, kept small for the form).
#define CFG_SSID_MAX   33   // 802.11 SSID <= 32 + NUL
#define CFG_PASS_MAX   64
#define CFG_URL_MAX    128  // https://<db>.upstash.io
#define CFG_KEY_MAX    48   // redis key, default "codexbar"
#define CFG_TOKEN_MAX  256  // Upstash read-only bearer token

// Remembered-WiFi list. Single NVS blob under key "wnets": one nvs_set_blob +
// nvs_commit makes an LRU rotate/evict atomic (matches the existing
// single-commit discipline; indexed keys would need a torn multi-key write).
// e[0] is always the most-recently-used; index increases toward LRU.
#define CFG_WIFI_MAX_ENTRIES 5
// 'WF01' — magic+version fused in one u32. There is NO blob-vN→vN+1 migration
// path (only the legacy single-SSID fold is handled): bumping this value
// makes every existing device treat its saved list as invalid and start from
// zero networks (it falls through to the captive portal). Only bump on an
// intentional, breaking layout change.
#define CFG_WIFI_BLOB_MAGIC  0x57463031u

typedef struct {
    char ssid[CFG_SSID_MAX];   // 33
    char pass[CFG_PASS_MAX];   // 64  (empty == open; rejected by WPA2 threshold)
} wifi_entry_t;

typedef struct {
    uint32_t     magic;        // CFG_WIFI_BLOB_MAGIC; anything else => migrate
    uint8_t      count;        // 0..CFG_WIFI_MAX_ENTRIES
    // struct_version repurposed from _pad[0] (sizeof unchanged; old blobs have 0 here).
    // Version 0 == current/baseline layout. On save, always write version 1.
    // Forward-compat rule: accept version 0 (old save) and 1 (current save);
    // reject any higher version (produced by a firmware we've never seen).
    // INVARIANT: ANY change to this struct layout MUST bump magic OR struct_version.
    // Bumping magic invalidates all existing blobs (treats them as legacy single-SSID).
    // Bumping struct_version allows graceful migration in blob_load.
    uint8_t      struct_version;  // was _pad[0]; old saved blobs read 0 here
    uint8_t      _pad[2];         // was _pad[1..2]; reserved, zero-filled on save
    wifi_entry_t e[CFG_WIFI_MAX_ENTRIES];   // e[0] = MRU
} wifi_creds_t;                // ~490 B; one NVS value entry

// Load NVS into memory (or defaults) and fold any legacy single-SSID creds
// into the "wnets" blob (idempotent; no erase, no forced portal). Call once,
// before display_init().
void config_store_init(void);

// --- Required by the vendored display.c (do not remove) ---
uint8_t config_store_get_brightness(void);
bool    config_store_get_display_flipped(void);

// --- Upstash credentials (independent of WiFi) ---

// true once an Upstash URL AND token are both present (key is optional;
// defaults to "codexbar"). WiFi is NOT considered here — see wifi_count().
bool config_store_has_upstash(void);

// Copy a field into buf (always NUL-terminated); returns strlen written.
size_t config_store_get_upstash_url(char *buf, size_t n);
size_t config_store_get_upstash_key(char *buf, size_t n);   // defaults to "codexbar"
size_t config_store_get_upstash_token(char *buf, size_t n);

// Persist ONLY the Upstash fields (single NVS commit). Empty key => default
// "codexbar". WiFi list untouched. Returns true on commit OK.
bool config_store_set_upstash(const char *url, const char *key,
                              const char *token);

// --- Remembered WiFi networks (MRU-ordered, max 5, LRU-evicted) ---

uint8_t config_store_wifi_count(void);

// Copy entry i (0 = MRU) into the caller buffers (always NUL-terminated).
// Returns the ssid strlen, or 0 if i >= count.
size_t config_store_wifi_get(uint8_t i, char *ssid, size_t sn,
                                         char *pass, size_t pn);

// Insert/replace ssid and promote it to MRU. Existing ssid (exact match) =>
// password overwritten in place + moved to e[0]. New + list full => the LRU
// entry (e[count-1]) is dropped. One nvs_set_blob + nvs_commit. Returns true
// on commit OK; false on bad args or NVS failure.
bool config_store_wifi_add_or_update(const char *ssid, const char *pass);

// Promote an already-present ssid to MRU WITHOUT changing its password
// (called on GOT_IP). No-op + true if already e[0]; false if not found.
// One commit only when the order actually changes.
bool config_store_wifi_promote(const char *ssid);

// (config_store_wifi_clear_all removed — confirmed zero callers.)

// --- One-shot "open captive portal to ADD a network" request ---
// Set by the non-destructive triple-tap / self-heal path; consumed exactly
// once at boot (clear-before-act => no boot-loop on power loss).
void config_store_request_portal(void);
bool config_store_take_portal_request(void);

// --- SoftAP PSK (random, generated once at first boot) ---
// Loads the stored PSK from NVS; if absent, generates a fresh random PSK,
// persists it, and returns it. out must be at least n bytes; n must be >= 9
// (8-char PSK + NUL). Returns true if the PSK is ready in out.
bool config_store_get_or_create_ap_psk(char *out, size_t n);

// (Legacy compatibility prototypes removed — confirmed zero callers in firmware tree.
//  The migration path in config_store_init() reads "ssid"/"pass" directly via
//  the internal get_str() helper and is not affected by this removal.)
