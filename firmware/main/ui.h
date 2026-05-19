// firmware/main/ui.h
//
// Owns the LVGL screen. ALL LVGL calls happen on the UI task (started by
// ui_start). Other tasks only call the thread-safe setters below, which copy
// state under a mutex and flag a redraw — no LVGL call ever runs off-task.
// (Same single-renderer discipline as clawd-tank's ui_manager.)
#pragma once

#include <stdint.h>
#include "stats_model.h"
#include "app_event.h"

// Create the LVGL UI task. Call once after display_init().
void ui_start(void);

// Progress-bar fill direction. Default (UI_BAR_INVERT_DEFAULT in ui.c) is
// INVERTED: 0% -> full bar, 100% -> empty bar (bars read as "headroom
// remaining"). Flip the compile-time default to change it globally, or call
// this at runtime (thread-safe) — wire it to a captive-portal / NVS setting
// later. Bar COLOR still tracks true usage % (red = high usage) either way.
void ui_set_bar_invert(bool on);

// --- input / navigation ---
//
// The swipe menu/submenu/card state machine lives here (mutex-protected `st`,
// mutated on the caller's task — same discipline as the setters below; no LVGL
// call ever runs off ui_task). fetch_task forwards EVERY input event here
// first and uses the result to decide whether its legacy behaviour runs.
typedef enum {
    UI_INPUT_PASS = 0,   // not consumed: caller may run refresh / triple-tap
    UI_INPUT_CONSUMED,   // nav handled it: caller must ignore this event
} ui_input_result_t;

// Process one touch/swipe event. Returns UI_INPUT_PASS only on the summary
// screen for a TAP (so refresh + triple-tap-reprovision keep working exactly
// as before); every menu/card interaction returns UI_INPUT_CONSUMED.
ui_input_result_t ui_handle_input(const app_evt_t *ev);

// --- thread-safe state setters (call from any task) ---

// Provisioning screen: show the SoftAP SSID + password to join.
void ui_set_provisioning(const char *ssid, const char *pass);

// One-line status banner (link state / errors), e.g. "WiFi: connecting…".
void ui_set_status(const char *text);

// Render the latest stats. `fetched_uptime_ms` = esp_timer ms at fetch, used
// for the "updated Ns ago" label (no NTP needed).
void ui_set_stats(const stats_t *s, int64_t fetched_uptime_ms);
