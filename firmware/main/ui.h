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
// The 2-state nav machine (NAV_SUMMARY ⇄ NAV_PAGE) lives here (mutex-protected
// `st`, mutated on the caller's task — same discipline as the setters below;
// no LVGL call ever runs off ui_task). fetch_task forwards EVERY input event
// here first and only acts (enter_portal) when the result is PASS.
//   summary: swipe up/down = scroll; tap a row = open its Cost page;
//            long-press = PASS (add-network portal); swipe-left = no-op.
//   page:    tap = cycle Cost↔Limit; swipe-left = back to summary.
typedef enum {
    UI_INPUT_PASS = 0,   // not consumed: caller may run enter_portal()
    UI_INPUT_CONSUMED,   // nav handled it: caller must ignore this event
} ui_input_result_t;

// Process one touch event. Returns UI_INPUT_PASS ONLY for a LONG_PRESS on the
// summary screen, or any event while in provisioning mode; everything else is
// CONSUMED by the nav machine.
// When the idle screensaver/dim fallback is active, the first stats-mode input
// restores saved navigation/brightness and is consumed; it never also performs
// navigation or passes LONG_PRESS through to setup.
ui_input_result_t ui_handle_input(const app_evt_t *ev);

// --- thread-safe state setters (call from any task) ---

// Provisioning screen: show the SoftAP SSID + password to join.
// `wifi_only` true => copy says "add a network, Upstash already set"
// (non-destructive add) instead of the full first-boot instructions.
void ui_set_provisioning(const char *ssid, const char *pass, bool wifi_only);

// One-line status banner (link state / errors), e.g. "WiFi: connecting…".
void ui_set_status(const char *text);

// Render the latest stats. `fetched_uptime_ms` = esp_timer ms at fetch, used
// for the "updated Ns ago" label (no NTP needed).
void ui_set_stats(const stats_t *s, int64_t fetched_uptime_ms);

// Force a full synchronous re-render so the display shadow framebuffer is
// complete, then signal the caller. Returns true on success, false on timeout.
// Blocks up to 2 s; call from screenshot_task only.
bool ui_capture_screenshot(void);
