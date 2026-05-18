// firmware/main/ui.h
//
// Owns the LVGL screen. ALL LVGL calls happen on the UI task (started by
// ui_start). Other tasks only call the thread-safe setters below, which copy
// state under a mutex and flag a redraw — no LVGL call ever runs off-task.
// (Same single-renderer discipline as clawd-tank's ui_manager.)
#pragma once

#include <stdint.h>
#include "stats_model.h"

// Create the LVGL UI task. Call once after display_init().
void ui_start(void);

// --- thread-safe state setters (call from any task) ---

// Provisioning screen: show the SoftAP SSID + password to join.
void ui_set_provisioning(const char *ssid, const char *pass);

// One-line status banner (link state / errors), e.g. "WiFi: connecting…".
void ui_set_status(const char *text);

// Render the latest stats. `fetched_uptime_ms` = esp_timer ms at fetch, used
// for the "updated Ns ago" label (no NTP needed).
void ui_set_stats(const stats_t *s, int64_t fetched_uptime_ms);
