// firmware/main/app_event.h
//
// Shared event type posted from input sources (touch) onto the app event
// queue and consumed by fetch_task. Decouples touch.c from any one consumer
// (clawd-tank's touch.c posted a BLE event; here it is provider-agnostic).
#pragma once

typedef enum {
    APP_EVT_TOUCH = 1,   // a debounced screen tap (force refresh / triple-tap reprovision)
} app_evt_type_t;

typedef struct {
    app_evt_type_t type;
} app_evt_t;
