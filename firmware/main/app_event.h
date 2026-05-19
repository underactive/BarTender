// firmware/main/app_event.h
//
// Shared input events posted from touch.c onto the app event queue. fetch_task
// forwards every event to ui_handle_input() (the nav state machine); it only
// acts (→ enter_portal) when that returns UI_INPUT_PASS, which now happens
// ONLY for a LONG_PRESS on the summary screen, or for any event while in
// provisioning mode. Every other event is consumed by the nav machine
// (tap = open/cycle a page; swipe up/down = scroll the summary; swipe-left =
// back). TAP keeps value 1 for wire-compat with the old APP_EVT_TOUCH.
#pragma once

#include <stdint.h>

typedef enum {
    APP_EVT_TAP        = 1,  // discrete tap; carries press x,y (was APP_EVT_TOUCH)
    APP_EVT_SWIPE_UP   = 2,  // finger moved toward the TOP of the screen
    APP_EVT_SWIPE_DOWN = 3,  // finger moved toward the BOTTOM of the screen
    APP_EVT_SWIPE_LEFT = 4,  // finger moved right→left (nav "back")
    APP_EVT_LONG_PRESS = 5,  // held ~1.5 s within slop; carries press x,y
} app_evt_type_t;

typedef struct {
    app_evt_type_t type;
    int16_t x, y;            // valid for TAP / LONG_PRESS (press point); 0 for swipes
} app_evt_t;
