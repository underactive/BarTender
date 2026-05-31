// firmware/main/fetch.h
//
// Periodic Upstash poll + input plumbing. Owns the fetch cadence; pushes
// results to the UI. Refresh is purely deadline-driven (every
// FETCH_INTERVAL_S); the nav machine (ui.c) consumes taps/swipes for
// scrolling and Cost/Limit pages. Only a LONG-PRESS (~1.5 s) on the summary
// pierces through (UI_INPUT_PASS) and opens the captive portal to ADD a WiFi
// network — NON-destructive: all remembered networks + Upstash are kept
// (net_wifi roams the ≤5 list, so re-provision is rarely needed). The gesture
// is honored even before WiFi has ever associated. If a fresh boot finds NO
// remembered network in range for CONNECT_GRACE_S, the device self-heals by
// opening that same add-network portal on its own.
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define FETCH_INTERVAL_S    60   // device poll cadence; faster than the 300 s macOS publisher so updates appear promptly
#define FETCH_RETRY_S       20   // first retry after a failed fetch
#define FETCH_RETRY_MAX_S  300   // backoff ceiling: a persistently failing store
                                 // is retried no slower than the 300 s publisher
                                 // cadence (FETCH_RETRY_S doubles per consecutive
                                 // failure, clamped here; resets on first success)
#define CONNECT_GRACE_S    180   // never-connected-this-boot: open the add-
                                 // network portal after this long ONLY IF
                                 // net_wifi_no_known_network() also holds
                                 // (>=2 sweeps, zero remembered SSIDs seen)

// Start the fetch task. `evt_q` carries app_evt_t (TAP/swipes/long-press)
// from touch.c.
void fetch_task_start(QueueHandle_t evt_q);
