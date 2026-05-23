---
date: 2026-05-22T21:34:31-0700
author: Eric Sison
commit: 16e4fd2
branch: master
repository: bartender
topic: "Idle active-provider screensaver"
tags: [plan, firmware-ui, screensaver, touch, display]
status: ready
parent: .rpiv/artifacts/designs/2026-05-22_21-25-01_idle-active-provider-screensaver.md
last_updated: 2026-05-22T21:34:31-0700
last_updated_by: Eric Sison
---

# Idle Active-Provider Screensaver Implementation Plan

## Overview

Build a firmware-only idle screensaver inside `firmware/main/ui.c` that reuses existing summary/provider card render paths and input boundaries. Provider activity is detected locally from stable provider metric deltas keyed by provider `id`; UI-task timing handles idle entry, page cycling, dim fallback, and brightness fade while wake input restores the saved navigation tuple and is consumed.

Design artifact: `.rpiv/artifacts/designs/2026-05-22_21-25-01_idle-active-provider-screensaver.md`

## Desired End State

```c
// Fetch continues unchanged.
ui_set_stats(&st, now_ms());

// User input continues through one boundary.
if (ui_handle_input(&ev) == UI_INPUT_PASS && ev.type == APP_EVT_LONG_PRESS)
    enter_portal();

// After idle, ui_task enters screensaver/dim locally; first input restores and
// returns UI_INPUT_CONSUMED, so fetch.c does not open the portal on wake.
```

## What We're NOT Doing

- No settings UI or persisted screensaver preferences.
- No host script, schema, payload, or parser shape changes.
- No new renderer module or duplicate provider-card UI.
- No changes to touch gesture classification.

## Phase 1: Silent Brightness Helper

### Overview

Add a shared internal brightness apply helper and expose `display_set_brightness_silent()` so the UI-task fade code can step brightness without per-frame logging.

### Changes Required:

#### 1. display.h — Declare silent brightness setter
**File**: `firmware/main/display.h`
**Changes**: Add function declaration for `display_set_brightness_silent(uint8_t duty)` with documentation explaining it's intended for frequent UI-owned fade steps.

```c
// Set PWM backlight brightness without logging; intended for frequent UI-owned
// fade steps. Use display_set_brightness() for user/config-driven changes.
void display_set_brightness_silent(uint8_t duty);
```

#### 2. display.c — Refactor brightness writes through shared helper
**File**: `firmware/main/display.c`
**Changes**: Extract LEDC duty write into `display_apply_brightness()`, then make both `display_set_brightness_silent()` and the existing `display_set_brightness()` call it. The logged setter keeps its existing log line.

```c
static void display_apply_brightness(uint8_t duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void display_set_brightness_silent(uint8_t duty)
{
    display_apply_brightness(duty);
}

void display_set_brightness(uint8_t duty)
{
    display_apply_brightness(duty);
    ESP_LOGI(TAG, "Brightness set to %u", duty);
}
```

### Success Criteria:

#### Automated Verification:
- [x] Firmware build passes: `cd firmware && idf.py build`
- [x] Brightness helper is declared once: `grep -R "display_set_brightness_silent" firmware/main/display.*`

#### Manual Verification:
- [ ] Existing brightness changes still log one `Brightness set` line when using `display_set_brightness()`.
- [ ] Silent helper can be used for repeated fade steps without emitting per-step brightness logs.

---

## Phase 2: Provider Activity Snapshots

### Overview

Add fixed-size provider activity slot tracking in `ui.c`, keyed by provider `id`. On each `ui_set_stats()` call, compare stable numeric metrics for `ok:true` providers against stored signatures; stamp `last_change_ms` when a difference is detected.

### Changes Required:

#### 1. ui.c — Add provider activity data types, helpers, and update hook
**File**: `firmware/main/ui.c`
**Changes**: Add `#include "display.h"` and `#include "config_store.h"`, screensaver constants, `saver_activity_t` type, activity array in `st`, activity helpers (`activity_slot`, `update_provider_activity_locked`, `provider_metric_sig`, `hash_mix_u32`, `pct_tenths`, `provider_has_limits_card`), and `find_provider_id`.

```c
#include "display.h"
#include "config_store.h"

#define SCREENSAVER_IDLE_MS     (5LL * 60LL * 1000LL)
#define SCREENSAVER_ACTIVE_MS   (8LL * 60LL * 60LL * 1000LL)
#define SCREENSAVER_PAGE_MS     (15LL * 1000LL)
#define SCREENSAVER_FADE_MS     700LL
#define SCREENSAVER_DIM_DUTY    8

typedef struct {
    char id[STATS_ID_MAX];
    bool seen;
    bool has_sig;
    uint32_t sig;
    int64_t last_change_ms;
} saver_activity_t;

// Add to `st` after `scroll`.
saver_activity_t activity[STATS_MAX_PROVIDERS];
bool saver_active, saver_dim_only;
nav_level_t saved_nav_level;
int saved_nav_provider;
char saved_nav_id[STATS_ID_MAX];
card_kind_t saved_nav_card;
int saved_scroll;
char saver_id[STATS_ID_MAX];
card_kind_t saver_card;
int64_t last_input_ms, saver_next_cycle_ms;
uint8_t saver_brightness, saver_target_brightness;
int64_t saver_fade_start_ms, saver_fade_end_ms;

static bool provider_has_limits_card(const stats_provider_t *p)
{
    return p->has_p || p->has_s || p->has_t || p->pct_hist_n > 0 ||
           (p->has_cost && p->extra_limit_c > 0);
}

static int pct_tenths(bool has, float v)
{
    if (!has) return -1;
    int t = (int)(v * 10.0f + 0.5f);
    if (t < 0) t = 0; else if (t > 1000) t = 1000;
    return t;
}

static uint32_t hash_mix_u32(uint32_t h, uint32_t v)
{
    h ^= v + 0x9e3779b9U + (h << 6) + (h >> 2);
    return h;
}

static uint32_t provider_metric_sig(const stats_provider_t *p)
{
    uint32_t h = 2166136261U;
    h = hash_mix_u32(h, p->ok ? 1U : 0U);
    h = hash_mix_u32(h, (uint32_t)pct_tenths(p->has_p, p->p));
    h = hash_mix_u32(h, (uint32_t)pct_tenths(p->has_s, p->s));
    h = hash_mix_u32(h, (uint32_t)pct_tenths(p->has_t, p->t));
    h = hash_mix_u32(h, p->has_cost ? 1U : 0U);
    if (p->has_cost) {
        h = hash_mix_u32(h, (uint32_t)p->cost_today_c);
        h = hash_mix_u32(h, (uint32_t)p->cost_month_c);
        h = hash_mix_u32(h, (uint32_t)p->tok_today);
        h = hash_mix_u32(h, (uint32_t)(p->tok_today >> 32));
        h = hash_mix_u32(h, (uint32_t)p->tok_month);
        h = hash_mix_u32(h, (uint32_t)(p->tok_month >> 32));
        h = hash_mix_u32(h, (uint32_t)p->extra_used_c);
        h = hash_mix_u32(h, (uint32_t)p->extra_limit_c);
        h = hash_mix_u32(h, (uint32_t)p->cost_week_c);
        h = hash_mix_u32(h, (uint32_t)p->credits_remaining_c);
        h = hash_mix_u32(h, (uint32_t)p->credits_limit_c);
        h = hash_mix_u32(h, (uint32_t)p->hist_n);
        for (int i = 0; i < p->hist_n && i < STATS_HIST_MAX; i++)
            h = hash_mix_u32(h, (uint32_t)p->hist[i]);
    }
    h = hash_mix_u32(h, (uint32_t)p->pct_hist_n);
    for (int i = 0; i < p->pct_hist_n && i < STATS_PCT_HIST_MAX; i++)
        h = hash_mix_u32(h, p->pct_hist[i]);
    return h;
}

static saver_activity_t *activity_slot(const char *id)
{
    int empty = -1;
    for (int i = 0; i < STATS_MAX_PROVIDERS; i++) {
        if (st.activity[i].seen && strcmp(st.activity[i].id, id) == 0) return &st.activity[i];
        if (!st.activity[i].seen && empty < 0) empty = i;
    }
    if (empty < 0) {
        int oldest = 0;
        for (int i = 1; i < STATS_MAX_PROVIDERS; i++)
            if (st.activity[i].last_change_ms < st.activity[oldest].last_change_ms) oldest = i;
        empty = oldest;
    }
    st.activity[empty] = (saver_activity_t){0};
    st.activity[empty].seen = true;
    strlcpy(st.activity[empty].id, id, sizeof st.activity[empty].id);
    return &st.activity[empty];
}

static void update_provider_activity_locked(const stats_t *s, int64_t now_ms)
{
    if (!s) return;
    for (int i = 0; i < s->n && i < STATS_MAX_PROVIDERS; i++) {
        const stats_provider_t *p = &s->p[i];
        if (!p->id[0] || is_hidden_provider(p->id) || !p->ok) continue;
        if (!p->has_cost && !provider_has_limits_card(p)) continue;
        uint32_t sig = provider_metric_sig(p);
        saver_activity_t *slot = activity_slot(p->id);
        if (!slot->has_sig) { slot->sig = sig; slot->has_sig = true; }
        else if (slot->sig != sig) { slot->last_change_ms = now_ms; slot->sig = sig; }
    }
}

static int find_provider_id(const char *id)
{
    for (int i = 0; id && i < st.stats.n && i < STATS_MAX_PROVIDERS; i++)
        if (strcmp(st.stats.p[i].id, id) == 0) return i;
    return -1;
}
```

#### 2. ui.c — Wire activity update into ui_set_stats
**File**: `firmware/main/ui.c`
**Changes**: Replace the existing stats copy with a version that also calls `update_provider_activity_locked()`.

```c
// ui_set_stats(): wrap copy with activity update.
if (s) { update_provider_activity_locked(s, fetched_uptime_ms); st.stats = *s; }
```

### Success Criteria:

#### Automated Verification:
- [x] Firmware build passes: `cd firmware && idf.py build`
- [x] No payload/schema files changed: `git diff -- docs/generated scripts firmware/main/stats_model.*`

#### Manual Verification:
- [ ] Hidden providers are not eligible for active-provider rotation.
- [ ] Reset hint string changes alone do not mark a provider active.

---

## Phase 3: Idle Entry, Page Cycle, and Fade/Dim Fallback

### Overview

Implement screensaver entry after idle timeout, active-provider page rotation every 15 s, and fade coordination from the UI task. If no active providers exist, fall back to dim-only mode (backlight dimmed, current page displayed).

### Changes Required:

#### 1. ui.c — Add saver entry, cycle, fade, and candidate helpers
**File**: `firmware/main/ui.c`
**Changes**: Add `saver_candidate_at`, `saver_start_fade_locked`, `saver_step_fade_locked`, `saver_enter_locked`, `saver_advance_locked`, and `provider_card_available`.

```c
static bool provider_card_available(const stats_provider_t *p, card_kind_t card)
{
    return card == CARD_COST ? p->has_cost : provider_has_limits_card(p);
}

static bool saver_candidate_at(int64_t now, int start, char *id, size_t id_n, card_kind_t *card)
{
    for (int step = 0; step < STATS_MAX_PROVIDERS; step++) {
        int i = (start + step) % STATS_MAX_PROVIDERS;
        saver_activity_t *a = &st.activity[i];
        if (!a->seen || !a->has_sig || a->last_change_ms <= 0) continue;
        if (now - a->last_change_ms > SCREENSAVER_ACTIVE_MS) continue;
        int pi = find_provider_id(a->id);
        if (pi < 0) continue;
        const stats_provider_t *p = &st.stats.p[pi];
        if (is_hidden_provider(p->id) || !p->ok) continue;
        if (p->has_cost) *card = CARD_COST;
        else if (provider_has_limits_card(p)) *card = CARD_LIMITS;
        else continue;
        strlcpy(id, a->id, id_n);
        return true;
    }
    return false;
}

static void saver_start_fade_locked(uint8_t from, uint8_t to, int64_t now)
{
    st.saver_brightness = from;
    st.saver_target_brightness = to;
    st.saver_fade_start_ms = now;
    st.saver_fade_end_ms = now + SCREENSAVER_FADE_MS;
    display_set_brightness_silent(from);
}

static void saver_step_fade_locked(int64_t now)
{
    if (st.saver_fade_end_ms <= 0) return;
    if (now >= st.saver_fade_end_ms) {
        display_set_brightness_silent(st.saver_target_brightness);
        st.saver_brightness = st.saver_target_brightness;
        st.saver_fade_end_ms = 0;
        return;
    }
    int64_t dur = st.saver_fade_end_ms - st.saver_fade_start_ms;
    int64_t el = now - st.saver_fade_start_ms;
    int duty = st.saver_brightness + (int)(((int)st.saver_target_brightness - (int)st.saver_brightness) * el / dur);
    if (duty < 0) duty = 0; else if (duty > 255) duty = 255;
    display_set_brightness_silent((uint8_t)duty);
}

static void saver_enter_locked(int64_t now)
{
    if (st.saver_active || st.mode != UI_STATS) return;
    st.saved_nav_level = st.nav_level;
    st.saved_nav_provider = st.nav_provider;
    strlcpy(st.saved_nav_id, st.nav_id, sizeof st.saved_nav_id);
    st.saved_nav_card = st.nav_card;
    st.saved_scroll = st.scroll;
    st.saver_active = true;
    st.saver_dim_only = !saver_candidate_at(now, 0, st.saver_id, sizeof st.saver_id, &st.saver_card);
    st.saver_next_cycle_ms = now + SCREENSAVER_PAGE_MS;
    saver_start_fade_locked(config_store_get_brightness(), st.saver_dim_only ? SCREENSAVER_DIM_DUTY : config_store_get_brightness(), now);
    if (!st.saver_dim_only) {
        int pi = find_provider_id(st.saver_id);
        if (pi >= 0) { st.nav_level = NAV_PAGE; st.nav_provider = pi; strlcpy(st.nav_id, st.saver_id, sizeof st.nav_id); st.nav_card = st.saver_card; }
    }
    st.dirty = true;
}

static void saver_advance_locked(int64_t now)
{
    if (!st.saver_active || st.saver_dim_only || now < st.saver_next_cycle_ms) return;
    int pi = find_provider_id(st.saver_id);
    if (pi >= 0 && st.saver_card == CARD_COST && provider_card_available(&st.stats.p[pi], CARD_LIMITS)) {
        st.saver_card = CARD_LIMITS;
    } else {
        int start = 0;
        for (int i = 0; i < STATS_MAX_PROVIDERS; i++) if (st.activity[i].seen && strcmp(st.activity[i].id, st.saver_id) == 0) { start = i + 1; break; }
        if (!saver_candidate_at(now, start, st.saver_id, sizeof st.saver_id, &st.saver_card)) { st.saver_dim_only = true; }
    }
    pi = find_provider_id(st.saver_id);
    if (!st.saver_dim_only && pi >= 0) { st.nav_provider = pi; strlcpy(st.nav_id, st.saver_id, sizeof st.nav_id); st.nav_card = st.saver_card; st.nav_level = NAV_PAGE; }
    st.saver_next_cycle_ms = now + SCREENSAVER_PAGE_MS;
    st.dirty = true;
}
```

#### 2. ui.c — Wire screensaver timer into ui_task
**File**: `firmware/main/ui.c`
**Changes**: After `now` is computed and before render in `ui_task()`, add screensaver timer steps.

```c
// ui_task(): after `now` is computed, before render.
if (st.mode != UI_STATS) { st.saver_active = false; st.saver_fade_end_ms = 0; }
saver_step_fade_locked(now);
if (st.mode == UI_STATS && !st.saver_active && st.last_input_ms > 0 && now - st.last_input_ms >= SCREENSAVER_IDLE_MS)
    saver_enter_locked(now);
saver_advance_locked(now);
```

### Success Criteria:

#### Automated Verification:
- [x] Firmware build passes: `cd firmware && idf.py build`
- [x] Screensaver constants and state are local to UI: `grep -n "SCREENSAVER" firmware/main/ui.c`

#### Manual Verification:
- [ ] With a recently changed visible provider, idle entry cycles meaningful provider cards by provider id.
- [ ] With no active providers, idle entry leaves current page rendered and dims the backlight.
- [ ] Page changes/fade do not block touch input or fetch cadence.

---

## Phase 4: Wake Restore and Docs

### Overview

Implement screensaver exit on first touch: save the navigation tuple at entry, consume the first wake input (including long press), restore brightness and nav state, and document the wake-only input contract in `ui.h`.

### Changes Required:

#### 1. ui.c — Add saver_exit_locked and wire wake into ui_handle_input
**File**: `firmware/main/ui.c`
**Changes**: Add `saver_exit_locked()` and modify `ui_handle_input()` to track `last_input_ms` and consume wake input before normal navigation.

```c
static void saver_exit_locked(int64_t now)
{
    if (!st.saver_active) return;
    st.saver_active = false;
    st.saver_dim_only = false;
    st.nav_level = st.saved_nav_level;
    st.nav_provider = st.saved_nav_provider;
    strlcpy(st.nav_id, st.saved_nav_id, sizeof st.nav_id);
    st.nav_card = st.saved_nav_card;
    st.scroll = st.saved_scroll;
    saver_start_fade_locked(SCREENSAVER_DIM_DUTY, config_store_get_brightness(), now);
    clamp_scroll();
    st.dirty = true;
}

// ui_handle_input(): after stats-mode check, before normal nav switch.
st.last_input_ms = esp_timer_get_time() / 1000;
if (st.saver_active) { saver_exit_locked(st.last_input_ms); r = UI_INPUT_CONSUMED; goto out; }
```

#### 2. ui.h — Document screensaver wake behavior
**File**: `firmware/main/ui.h`
**Changes**: Add documentation block describing the screensaver wake contract for `ui_handle_input()`.

```c
// When the idle screensaver/dim fallback is active, the first stats-mode input
// restores saved navigation/brightness and is consumed; it never also performs
// navigation or passes LONG_PRESS through to setup.
```

### Success Criteria:

#### Automated Verification:
- [x] Firmware build passes: `cd firmware && idf.py build`
- [x] `ui_handle_input()` consumes screensaver wake before long-press pass-through: `grep -n "saver_exit_locked" firmware/main/ui.c`

#### Manual Verification:
- [ ] First tap/swipe/long-press while screensaver/dim is active only wakes and restores saved navigation.
- [ ] Long-press on summary still opens setup when screensaver is not active.
- [ ] Provider page restored by `nav_id` after provider reorder/drop, falling back to summary if missing.

---

## Testing Strategy

### Automated:
- `cd firmware && idf.py build` after each phase.
- `grep -R "display_set_brightness_silent" firmware/main/display.*` for declaration uniqueness.
- `git diff -- docs/generated scripts firmware/main/stats_model.*` to confirm no payload/schema changes.
- `grep -n "SCREENSAVER" firmware/main/ui.c` for constant locality.
- `grep -n "saver_exit_locked" firmware/main/ui.c` for wake-consume placement.

### Manual Testing Steps:
1. Verify existing brightness changes still log one `Brightness set` line when using `display_set_brightness()`.
2. Verify silent helper does not emit per-step brightness logs during fade.
3. Verify hidden providers are not eligible for active-provider rotation.
4. Verify reset hint string changes alone do not mark a provider active.
5. With a recently changed visible provider, verify idle entry cycles meaningful provider cards by provider id.
6. With no active providers, verify idle entry leaves current page rendered and dims the backlight.
7. Verify page changes/fade do not block touch input or fetch cadence.
8. Verify first tap/swipe/long-press while screensaver/dim is active only wakes and restores saved navigation.
9. Verify long-press on summary still opens setup when screensaver is not active.
10. Verify provider page restored by `nav_id` after provider reorder/drop, falling back to summary if missing.

## Performance Considerations

All scans are bounded by `STATS_MAX_PROVIDERS` and fixed history caps. Fade/page-cycle work is scheduled from the existing 5 ms UI task loop and only marks dirty or sets LEDC duty, avoiding blocking fetch/input handling.

## Migration Notes

No persisted state, schema, payload, or host migration. Activity memory is session-only and resets on reboot.

## Plan Review (Step 4)

_Independent post-finalization review by artifact-code-reviewer and artifact-coverage-reviewer subagents. Findings triaged at Step 5._

| source   | plan-loc          | codebase-loc                | severity   | dimension             | finding   | recommendation   | resolution         |
| -------- | ----------------- | --------------------------- | ---------- | --------------------- | --------- | ---------------- | ------------------ |
| code     | Phase 3 §2 (ui.c) | firmware/main/ui.c:1178-1190 | suggestion | code-quality          | The screensaver timer wiring resets `st.saver_active = false` when `st.mode != UI_STATS` but does not also reset `st.saver_fade_end_ms = 0`, so if a fade was in-flight when mode switches to provisioning the fade continues running (harmlessly — it just steps brightness while provisioning chrome is shown). | Add `st.saver_fade_end_ms = 0` alongside `st.saver_active = false` inside the `if (st.mode != UI_STATS)` guard. | applied: added fade end reset |
| code     | Phase 2 §1 (ui.c) | <n/a>                       | suggestion | code-quality          | The `saver_activity_t` slot count equals `STATS_MAX_PROVIDERS` (12); when all 12 slots are occupied and a new id arrives, `activity_slot` silently evicts slot 0 rather than the least-recently-seen provider, which could bounce a continuously active provider. | Evict the slot with the oldest `last_change_ms` instead of index 0, or keep a simple LRU counter on each slot. | applied: LRU eviction by oldest last_change_ms |
| coverage | ## Testing Strategy §1–§15 | <n/a>                       | none      | verification-coverage  | All verification intents are covered. | <n/a> | no action needed |

## Developer Context

- **Step 5**: 2 suggestions from artifact-code-reviewer both applied:
  - Phase 3 §2: added `st.saver_fade_end_ms = 0` alongside `st.saver_active = false` in the mode guard.
  - Phase 2 §1: replaced index-0 eviction with LRU by oldest `last_change_ms` in `activity_slot()`.
- artifact-coverage-reviewer: all verification intents covered, no findings.

## References

- Design: `.rpiv/artifacts/designs/2026-05-22_21-25-01_idle-active-provider-screensaver.md`
- Research: `.rpiv/artifacts/research/2026-05-22_21-09-22_idle-active-provider-screensaver.md`
- Discovery: `.rpiv/artifacts/discover/2026-05-22_21-00-11_idle-active-provider-screensaver.md`
- Prior research: `.rpiv/artifacts/research/2026-05-22_13-48-57_pi-agent-stats.md`