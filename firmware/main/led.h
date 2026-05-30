// firmware/main/led.h
#pragma once
#include <stdint.h>

void led_init(void);
void led_set_provider(const char *id);
void led_off(void);

// Summary page: smooth crossfade across visible providers (5 s per step).
void led_summary_reset(void);
void led_summary_cycle_tick(int64_t now_ms, const char *const *ids, int n);
