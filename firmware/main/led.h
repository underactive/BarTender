// firmware/main/led.h
#pragma once
#include <stdbool.h>
#include <stdint.h>

void led_init(void);
void led_set_provider(const char *id);
void led_off(void);

// Summary page: smooth crossfade across visible providers (5 s per step).
void led_summary_reset(void);
void led_summary_cycle_tick(int64_t now_ms, const char *const *ids, int n);

// Screensaver transitions: enable before entering screensaver so that
// led_set_provider() interpolates from the current colour instead of
// snapping.  Disable on exit; call led_transition_tick() each render
// while a transition is active.
void led_transition_enable(void);
void led_transition_disable(void);
bool led_is_transitioning(void);
void led_transition_tick(int64_t now_ms);
