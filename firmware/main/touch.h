#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Initialize the FT6336G capacitive touch controller on the fnk0104 board.
// A dedicated task polls the panel at ~50 Hz; each debounced touch-down posts
// an APP_EVT_TOUCH (see app_event.h) to evt_queue.
//
// No-op on boards without touch (compiled out via BOARD_HAS_TOUCH).
void touch_init(QueueHandle_t evt_queue);
