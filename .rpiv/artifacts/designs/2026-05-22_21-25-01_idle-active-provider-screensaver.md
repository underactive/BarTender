---
date: 2026-05-22T21:25:01-0700
author: Eric Sison
commit: 16e4fd2
branch: master
repository: bartender
topic: "Idle active-provider screensaver"
tags: [design, firmware-ui, screensaver, touch, display]
status: ready
parent: .rpiv/artifacts/research/2026-05-22_21-09-22_idle-active-provider-screensaver.md
last_updated: 2026-05-22T21:25:01-0700
last_updated_by: Eric Sison
---

# Design: Idle Active-Provider Screensaver

## Summary
Build a firmware-only idle screensaver inside `firmware/main/ui.c` that reuses existing summary/provider card render paths and input boundaries. Provider activity is detected locally from stable provider metric deltas keyed by provider `id`; UI-task timing handles idle entry, page cycling, dim fallback, and brightness fade while wake input restores the saved navigation tuple and is consumed.

## Requirements
- Enter screensaver after idle time and show recently active providers from the last 8 hours of observed session data.
- Activity is firmware-local: compare stable numeric fields for `ok:true` providers; do not change host scripts or Upstash payload.
- Include summary/dim behavior, Today/Cost only when cost exists, and Limits only when meaningful limit data exists.
- Mirror summary hidden-provider filtering (`ollama`, `opencode`, `opencodego`).
- Consume first wake touch and restore the page/scroll/card visible before screensaver entry.
- Coordinate fade/page cycling from the UI task; avoid brightness log spam during fade steps.

## Current State Analysis
### Key Discoveries
- `firmware/main/ui.c:35-52` centralizes stats, dirty state, and navigation fields under `s_mtx`.
- `firmware/main/ui.c:1015-1066` re-resolves provider pages by `nav_id` and dispatches to existing card renderers.
- `firmware/main/ui.c:1182-1216` is the UI-task timing/render loop and existing place for periodic dirty marking.
- `firmware/main/ui.c:1270-1277` is the old/new stats boundary for activity comparisons.
- `firmware/main/ui.c:1287-1335` is the input boundary; it mutates only state and returns PASS/CONSUMED.
- `firmware/main/display.c:269-274` has a logged brightness setter; fade needs a silent path.

## Scope
### Building
- Silent display brightness helper for UI-task fade steps.
- Provider snapshot/signature tracking in `ui.c` keyed by provider id.
- Screensaver state in `ui.c`: saved navigation, active/dim mode, cycle state, fade state.
- Idle entry, active-provider page rotation, dim fallback, and wake/restore behavior.
- `ui.h` documentation update for wake-only screensaver semantics.

### Not Building
- No settings UI or persisted screensaver preferences.
- No host script, schema, payload, or parser shape changes.
- No new renderer module or duplicate provider-card UI.
- No changes to touch gesture classification.

## Decisions
### Keep screensaver in `ui.c`
Decision: implement screensaver as UI-owned navigation overlay/state in `firmware/main/ui.c`, reusing existing render/input/timer patterns.
Evidence: `firmware/main/ui.c:35-52`, `firmware/main/ui.c:1015-1066`, `firmware/main/ui.c:1182-1216`.

### Use provider-id activity snapshots
Decision: keep fixed-size provider activity slots in `ui.c`, keyed by `stats_provider_t.id`, stamping `last_change_ms` when stable comparable metrics differ for an `ok:true` provider.
Evidence: provider identity navigation already uses ids at `firmware/main/ui.c:47-49`; metric fields are available at `firmware/main/stats_model.h:26-59`.

### Reuse card availability predicates
Decision: Cost pages require `has_cost`; Limits pages require `has_p || has_s || has_t || pct_hist_n > 0 || (has_cost && extra_limit_c > 0)`; hidden providers are excluded.
Evidence: hidden provider helper at `firmware/main/ui.c:124-132`, Cost placeholder at `firmware/main/ui.c:801-812`, Limits render branches at `firmware/main/ui.c:896-1007`.

### Add silent brightness helper
Ambiguity: fade needs repeated brightness duty updates but `display_set_brightness()` logs every call.
Explored: (A) silent helper — smooth fade without log spam; (B) discrete dim only — simpler but weaker UX; (C) logged fade — noisy.
Decision: add `display_set_brightness_silent(uint8_t duty)` and make the existing logged setter delegate to a shared internal helper.
Developer checkpoint: selected “Silent fade helper (Recommended)”. Evidence: `firmware/main/display.c:269-274`.

## Architecture
### firmware/main/display.h:1-20 — MODIFY
Expose silent brightness setter for UI-task fade.
```c
// Set PWM backlight brightness without logging; intended for frequent UI-owned
// fade steps. Use display_set_brightness() for user/config-driven changes.
void display_set_brightness_silent(uint8_t duty);
```

### firmware/main/display.c:269-274 — MODIFY
Refactor brightness writes through a shared helper.
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

### firmware/main/ui.c:1-1340 — MODIFY
Add `#include "display.h"` and `#include "config_store.h"`, constants, provider activity helpers, screensaver state, UI-task timers, and wake handling. Implement should merge these blocks into existing `ui.c`.
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
    if (empty < 0) empty = 0;
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

// ui_set_stats(): wrap copy with activity update.
if (s) { update_provider_activity_locked(s, fetched_uptime_ms); st.stats = *s; }

// ui_task(): after `now` is computed, before render.
if (st.mode != UI_STATS) st.saver_active = false;
saver_step_fade_locked(now);
if (st.mode == UI_STATS && !st.saver_active && st.last_input_ms > 0 && now - st.last_input_ms >= SCREENSAVER_IDLE_MS)
    saver_enter_locked(now);
saver_advance_locked(now);

// ui_handle_input(): after stats-mode check, before normal nav switch.
st.last_input_ms = esp_timer_get_time() / 1000;
if (st.saver_active) { saver_exit_locked(st.last_input_ms); r = UI_INPUT_CONSUMED; goto out; }
```


### firmware/main/ui.h:20-45 — MODIFY
Document screensaver wake behavior on `ui_handle_input()`.
```c
// When the idle screensaver/dim fallback is active, the first stats-mode input
// restores saved navigation/brightness and is consumed; it never also performs
// navigation or passes LONG_PRESS through to setup.
```

## Slices
### Slice 1: Silent brightness helper

**Files**: `firmware/main/display.h`, `firmware/main/display.c`

#### Automated Verification:
- [ ] Firmware build passes: `cd firmware && idf.py build`
- [ ] Brightness helper is declared once: `grep -R "display_set_brightness_silent" firmware/main/display.*`

#### Manual Verification:
- [ ] Existing brightness changes still log one `Brightness set` line when using `display_set_brightness()`.
- [ ] Silent helper can be used for repeated fade steps without emitting per-step brightness logs.

### Slice 2: Provider activity snapshots

**Files**: `firmware/main/ui.c`

#### Automated Verification:
- [ ] Firmware build passes: `cd firmware && idf.py build`
- [ ] No payload/schema files changed: `git diff -- docs/generated scripts firmware/main/stats_model.*`

#### Manual Verification:
- [ ] Hidden providers are not eligible for active-provider rotation.
- [ ] Reset hint string changes alone do not mark a provider active.

### Slice 3: Idle entry, page cycle, and fade/dim fallback

**Files**: `firmware/main/ui.c`

#### Automated Verification:
- [ ] Firmware build passes: `cd firmware && idf.py build`
- [ ] Screensaver constants and state are local to UI: `grep -n "SCREENSAVER" firmware/main/ui.c`

#### Manual Verification:
- [ ] With a recently changed visible provider, idle entry cycles meaningful provider cards by provider id.
- [ ] With no active providers, idle entry leaves current page rendered and dims the backlight.
- [ ] Page changes/fade do not block touch input or fetch cadence.

### Slice 4: Wake restore and docs

**Files**: `firmware/main/ui.c`, `firmware/main/ui.h`

#### Automated Verification:
- [ ] Firmware build passes: `cd firmware && idf.py build`
- [ ] `ui_handle_input()` consumes screensaver wake before long-press pass-through: `grep -n "saver_exit_locked" firmware/main/ui.c`

#### Manual Verification:
- [ ] First tap/swipe/long-press while screensaver/dim is active only wakes and restores saved navigation.
- [ ] Long-press on summary still opens setup when screensaver is not active.
- [ ] Provider page restored by `nav_id` after provider reorder/drop, falling back to summary if missing.

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

## File Map
```text
firmware/main/display.h  # MODIFY — silent brightness helper declaration
firmware/main/display.c  # MODIFY — shared brightness apply helper + silent setter
firmware/main/ui.c       # MODIFY — screensaver state, activity tracking, cycle/fade/wake logic
firmware/main/ui.h       # MODIFY — input contract documentation
```

## Ordering Constraints
1. Add brightness helper before UI fade calls it.
2. Add provider activity snapshots before idle page rotation consumes activity slots.
3. Add idle/page-cycle/fade logic before wake/restore behavior depends on active/dim state.
4. Documentation follows behavior.

## Verification Notes
- Verify `cd firmware && idf.py build` after each phase.
- Verify no host scripts, schema, or parser payload shape changed.
- Inspect `display_set_brightness_silent` usage to ensure fade steps do not log per frame.
- Validate hidden providers and unavailable cards are skipped.
- Validate wake touch is consumed, including long press, while screensaver/dim is active.
- Validate provider identity uses ids, not stale indexes, across refresh reorder/drop.

## Performance Considerations
All scans are bounded by `STATS_MAX_PROVIDERS` and fixed history caps. Fade/page-cycle work is scheduled from the existing 5 ms UI task loop and only marks dirty or sets LEDC duty, avoiding blocking fetch/input handling.

## Migration Notes
No persisted state, schema, payload, or host migration. Activity memory is session-only and resets on reboot.

## Pattern References
- `firmware/main/ui.c:35-52` — shared UI state pattern.
- `firmware/main/ui.c:1015-1066` — id-based provider render dispatch.
- `firmware/main/ui.c:1182-1216` — UI-task timer/dirty render pattern.
- `firmware/main/ui.c:1287-1335` — input boundary and pass/consume contract.
- `firmware/main/display.c:269-274` — runtime brightness setter to extend.

## Developer Context
**Q:** `display_set_brightness()` logs every call (`firmware/main/display.c:269-274`), while the research asks for nonblocking UI-task fade coordination. Which backlight approach should the design use?
**A:** Silent fade helper (Recommended).

**Q:** Design summary with decisions and scope. Ready to proceed to decomposition?
**A:** Proceed (Recommended).

**Q:** 4 slices for Idle active-provider screensaver. Approve decomposition?
**A:** Approve (Recommended).

**Q:** Slice 2 verifier found first observation should establish a baseline, not count as activity. Proceed with `has_sig` revision?
**A:** Approve revision (Recommended).

## Design History
- Slice 1: Silent brightness helper — approved as generated
- Slice 2: Provider activity snapshots — approved revised: first observation establishes baseline via `has_sig`; later deltas stamp `last_change_ms`
- Slice 3: Idle entry, page cycle, and fade/dim fallback — approved as generated
- Slice 4: Wake restore and docs — approved as generated

## References
- `.rpiv/artifacts/research/2026-05-22_21-09-22_idle-active-provider-screensaver.md`
- `.rpiv/artifacts/discover/2026-05-22_21-00-11_idle-active-provider-screensaver.md`
- `.rpiv/artifacts/research/2026-05-22_13-48-57_pi-agent-stats.md`
