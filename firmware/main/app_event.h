// firmware/main/app_event.h
//
// Shared input events posted from touch.c onto the app event queue. fetch_task
// forwards every event to ui_handle_input() (the nav state machine) and only
// runs the legacy refresh / triple-tap-reprovision logic when that returns
// UI_INPUT_PASS (i.e. on the summary screen). Clean migration from the old
// single APP_EVT_TOUCH: TAP keeps value 1, swipes are new.
#pragma once

#include <stdint.h>

typedef enum {
    APP_EVT_TAP        = 1,  // discrete tap; carries press x,y (was APP_EVT_TOUCH)
    APP_EVT_SWIPE_UP   = 2,  // finger moved toward the TOP of the screen
    APP_EVT_SWIPE_DOWN = 3,  // finger moved toward the BOTTOM of the screen
} app_evt_type_t;

typedef struct {
    app_evt_type_t type;
    int16_t x, y;            // valid for APP_EVT_TAP (press point); 0 for swipes
} app_evt_t;
