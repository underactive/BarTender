// firmware/main/wifi_lru.h
//
// Header-only, pure-C LRU helpers for the remembered-WiFi array.
// NO ESP-IDF / NVS dependencies — only <string.h> / <stdint.h> / <stdbool.h>.
// Intended to be included by config_store.c (which owns the NVS I/O around it)
// AND by the host-side unit-test.
//
// Ordering contract (mirrors config_store.c exactly):
//   e[0] = most-recently-used (MRU)
//   e[count-1] = least-recently-used (LRU), first to be evicted
//
// All functions operate on a caller-supplied array + count + max so they carry
// no hidden state and can be called from unit tests without any ESP-IDF shim.
#pragma once

#include "config_store.h"   // wifi_creds_t, wifi_entry_t, CFG_SSID_MAX, CFG_WIFI_MAX_ENTRIES
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// wifi_lru_find
// ---------------------------------------------------------------------------
// Find the first slot whose SSID exactly matches `ssid` (strncmp, CFG_SSID_MAX).
// Returns the index [0, count) on match, or -1 if absent.
static inline int wifi_lru_find(const wifi_creds_t *w, const char *ssid)
{
    for (uint8_t i = 0; i < w->count; i++)
        if (strncmp(w->e[i].ssid, ssid, CFG_SSID_MAX) == 0) return (int)i;
    return -1;
}

// ---------------------------------------------------------------------------
// wifi_lru_promote
// ---------------------------------------------------------------------------
// Move the entry at index j to e[0] by shifting e[j-1..0] one slot toward the
// LRU end.  Callers must ensure 0 < j < count.  Does NOT touch count or any
// field other than the shifted entries.  Password of the promoted entry is
// preserved unchanged.
static inline void wifi_lru_promote(wifi_creds_t *w, int j)
{
    wifi_entry_t saved = w->e[j];
    for (int k = j; k > 0; k--) w->e[k] = w->e[k - 1];
    w->e[0] = saved;
}

// ---------------------------------------------------------------------------
// wifi_lru_upsert
// ---------------------------------------------------------------------------
// Insert or update an entry while maintaining MRU ordering and LRU eviction:
//
//   • Duplicate SSID (found at index j):
//       – Shift e[j-1..0] one slot toward LRU end.
//       – Place the new entry (with the new password) at e[0].
//       – count is NOT incremented.
//
//   • New SSID, list not full (count < max):
//       – Shift e[count-1..0] one slot toward LRU end.
//       – Place new entry at e[0].
//       – count++.
//
//   • New SSID, list full (count == max):
//       – Keep = max - 1; shift e[keep-1..0] one slot toward LRU end.
//         (This overwrites the slot at index max-1, which was the LRU entry.)
//       – Place new entry at e[0].
//       – count remains max (LRU was evicted, a new MRU takes its place).
//
//   After either branch, slots e[count..max-1] are zeroed (no stale data).
//   w->magic is reset to CFG_WIFI_BLOB_MAGIC.
//
// `ne` is the fully-populated new entry (ssid + pass already copied in).
// `max` should be CFG_WIFI_MAX_ENTRIES; callers may pass a smaller value in
// tests to exercise the full/evict path with a tiny array.
static inline void wifi_lru_upsert(wifi_creds_t *w, const wifi_entry_t *ne,
                                    uint8_t max)
{
    int j = wifi_lru_find(w, ne->ssid);
    if (j >= 0) {
        // Promote existing slot to MRU and overwrite password.
        for (int k = j; k > 0; k--) w->e[k] = w->e[k - 1];
        w->e[0] = *ne;                   // count unchanged
    } else {
        // Prepend; if full, the shift drops the LRU entry at e[count-1].
        uint8_t keep = w->count;
        if (keep == max) keep = (uint8_t)(max - 1);
        for (int k = keep; k > 0; k--) w->e[k] = w->e[k - 1];
        w->e[0] = *ne;
        if (w->count < max) w->count++;
    }
    // Zero-fill tail slots so no stale password residue lingers.
    for (uint8_t i = w->count; i < max; i++)
        memset(&w->e[i], 0, sizeof w->e[i]);
    w->magic = CFG_WIFI_BLOB_MAGIC;
}
