#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>

// Initialize the FT6336G capacitive touch controller on the fnk0104 board.
// A dedicated task polls the panel at ~50 Hz; each debounced press posts one
// app_evt_t (TAP / SWIPE_UP / SWIPE_DOWN / SWIPE_LEFT / LONG_PRESS, see
// app_event.h) to evt_queue.
//
// No-op on boards without touch (compiled out via BOARD_HAS_TOUCH).
void touch_init(QueueHandle_t evt_queue);

// Sync touch mirror flags to match a 180° display flip. Must be called
// whenever display_set_flipped() changes the panel orientation so that tap
// coordinates remain in the same frame as the rendered layout.
// No-op on boards without touch.
void touch_set_flipped(bool flipped);
