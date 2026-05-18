// firmware/main/fetch.h
//
// Periodic Upstash poll + touch handling. Owns the fetch cadence; pushes
// results to the UI. A single tap forces an immediate refresh; a triple-tap
// within 2 s wipes credentials and reboots into the captive portal.
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define FETCH_INTERVAL_S   300   // matches the macOS publisher cadence
#define FETCH_RETRY_S       20   // after a failed fetch

// Start the fetch task. `evt_q` carries APP_EVT_TOUCH from touch.c.
void fetch_task_start(QueueHandle_t evt_q);
