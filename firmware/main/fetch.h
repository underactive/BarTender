// firmware/main/fetch.h
//
// Periodic Upstash poll + touch handling. Owns the fetch cadence; pushes
// results to the UI. A single tap forces an immediate refresh; a triple-tap
// within 2 s opens the captive portal to ADD a WiFi network — NON-destructive:
// all remembered networks + Upstash are kept (net_wifi roams the ≤5 list, so
// re-provision is rarely needed). The gesture is honored even before WiFi has
// ever associated. If a fresh boot finds NO remembered network in range for
// CONNECT_GRACE_S, the device self-heals by opening that same add-network
// portal on its own (a relocated toy with no one to triple-tap).
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define FETCH_INTERVAL_S   300   // matches the macOS publisher cadence
#define FETCH_RETRY_S       20   // after a failed fetch
#define CONNECT_GRACE_S    180   // never-connected-this-boot: open the add-
                                 // network portal after this long ONLY IF
                                 // net_wifi_no_known_network() also holds
                                 // (>=2 sweeps, zero remembered SSIDs seen)

// Start the fetch task. `evt_q` carries APP_EVT_TOUCH from touch.c.
void fetch_task_start(QueueHandle_t evt_q);
