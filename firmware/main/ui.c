// firmware/main/ui.c
//
// Top-level UI lifecycle: owns the shared `st` state + mutex, runs the ui_task
// render loop, the thread-safe public setters, and the nav state machine. The
// heavy lifting was split out: pure formatting -> ui_format.c, LVGL widget
// build/draw -> ui_render.c, screensaver/backlight -> ui_screensaver.c. All
// four translation units share ui_internal.h (private), and ui.h (public) is
// the only interface callers see.
#include "ui.h"
#include "ui_internal.h"
#include "provider_colors.h"
#include "led.h"
#include "config_store.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

// ── Shared state (declared extern in ui_internal.h; owned here) ──────────────
// Written by any task under s_mtx, consumed only by ui_task.
SemaphoreHandle_t s_mtx;
SemaphoreHandle_t s_shot_sem;  // binary; given by ui_task after snapshot render
struct ui_state st;

// ── scrollable-summary geometry (pure reads of st + cached s_scr_h; safe to
// call from ui_handle_input off ui_task — no LVGL, mutate only st.scroll under
// s_mtx, same discipline as the rest of the nav state) ────────────────────────

// How many provider rows fit on screen below the title/status band.
int summary_vis_rows(void)
{
    const ui_page_grid_t g = ui_grid_from_height(s_scr_w, s_scr_h);
    return summary_vis_rows_from_grid(&g);
}

// Summary rows are compacted over visible providers: hidden providers must not
// consume scroll slots or tap targets, otherwise later visible providers (Pi)
// can be pushed below blank rows.
int summary_visible_count(void)
{
    int n = 0;
    for (int i = 0; i < st.stats.n && i < STATS_MAX_PROVIDERS; i++)
        if (!is_hidden_provider(st.stats.p[i].id)) n++;
    return n;
}

// Map a compact visible-list index back to the underlying stats.p[] index.
int summary_provider_at(int visible_idx)
{
    if (visible_idx < 0) return -1;
    int seen = 0;
    for (int i = 0; i < st.stats.n && i < STATS_MAX_PROVIDERS; i++) {
        if (is_hidden_provider(st.stats.p[i].id)) continue;
        if (seen == visible_idx) return i;
        seen++;
    }
    return -1;
}

// Pin st.scroll into [0, max(0, visible_count - visible_rows)].
void clamp_scroll(void)
{
    int max = summary_visible_count() - summary_vis_rows();
    if (max < 0)          max = 0;
    if (st.scroll > max)  st.scroll = max;
    if (st.scroll < 0)    st.scroll = 0;
}

// Tap y -> provider index (accounting for compact visible-list scroll), or -1
// on a miss / the inter-row gap. MUST mirror summary render geometry.
int summary_hit_test(int y)
{
    const ui_page_grid_t g = ui_grid_from_height(s_scr_w, s_scr_h);
    if (y < g.content.y || y >= g.content.y + g.content.h) return -1;
    const int grid_row = (y - g.content.y) / g.cell_h;
    if (grid_row < UI_SUMMARY_TOP_ROWS) return -1;
    const int slot = grid_row - UI_SUMMARY_TOP_ROWS;
    if (slot < 0 || slot >= summary_vis_rows_from_grid(&g)) return -1;
    if ((y - g.content.y) % g.cell_h > g.cell_h - UI_SUMMARY_GAP) return -1;
    return summary_provider_at(st.scroll + slot);
}

// Sum of today's tokens across all summary-visible providers.
int64_t summary_tok_today_total(void)
{
    int64_t sum = 0;
    for (int i = 0; i < st.stats.n; i++) {
        const stats_provider_t *p = &st.stats.p[i];
        if (is_hidden_provider(p->id)) continue;
        sum += provider_tok_today(p);
    }
    return sum;
}

// Summary-page rear LED: crossfade across visible providers (led.c).
static void led_summary_tick_locked(int64_t now_ms)
{
    const char *ids[STATS_MAX_PROVIDERS];
    int n = 0;
    for (int i = 0; i < st.stats.n && i < STATS_MAX_PROVIDERS; i++) {
        if (is_hidden_provider(st.stats.p[i].id)) continue;
        if (!prov_color_hex(st.stats.p[i].id)) continue;
        ids[n++] = st.stats.p[i].id;
    }
    led_summary_cycle_tick(now_ms, ids, n);
}

static void ui_task(void *arg)
{
    (void)arg;
    build_widgets();
    int64_t next_age = 0;
    int64_t next_led  = 0;   // LED cadence gate: only update LED every LED_TICK_INTERVAL_MS
    while (1) {
        bool do_shot = false;
        // Audit State§HIGH: block briefly for the mutex rather than skipping
        // the render entirely (take(0)) under setter contention.
        if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5)) == pdTRUE) {
            int64_t now = esp_timer_get_time() / 1000;
            // Screensaver timers.
            if (st.mode != UI_STATS) { st.saver_active = false; st.saver_fade_end_ms = 0; st.saver_transitioning = false; st.saver_show_summary = false; st.saver_next_show_summary = false; }
            boot_step_fade_locked(now);
            if (st.boot_fade == BOOT_FADE_NONE)
                saver_step_fade_locked(now);
            if (st.mode == UI_STATS && !st.saver_active && !st.locked && st.last_input_ms > 0 && now - st.last_input_ms >= SCREENSAVER_IDLE_MS) {
                saver_enter_locked(now);
                led_transition_enable();
            }
            saver_advance_locked(now);
            // Re-render every 10 s so the "updated Ns ago" counter ticks even
            // without new data. render() recomputes the age from st.fetched_ms,
            // so this is always accurate (no gap). Audit (Perf§MED): the age
            // suffix renders ONLY on the summary page (the NAV_PAGE branch in
            // render() returns before it), so gate the tick to NAV_SUMMARY —
            // otherwise a drilled-in card redraws identical pixels (full
            // render_card() + lv_chart_refresh) every 10 s for nothing.
            if (st.mode == UI_STATS && st.nav_level == NAV_SUMMARY &&
                st.fetched_ms > 0 && now >= next_age) {
                next_age = now + AGE_TICK_MS;
                st.dirty = true;
            }
            if (st.mode == UI_STATS && st.nav_level == NAV_SUMMARY && now >= next_led) {
                next_led = now + LED_TICK_INTERVAL_MS;
                led_summary_tick_locked(now);
            }
            // Screensaver rear-LED: smooth colour transition on provider change.
            if (st.mode == UI_STATS && st.saver_active &&
                st.nav_level == NAV_PAGE && now >= next_led) {
                next_led = now + LED_TICK_INTERVAL_MS;
                led_transition_tick(now);
            }
            if (st.dirty) { render(); st.dirty = false; }
            do_shot = st.shot_req;
            if (do_shot) st.shot_req = false;
            xSemaphoreGive(s_mtx);
        }
        lv_timer_handler();
        lv_obj_t *scr = ui_active_screen();
        if (do_shot && scr) {
            // Force a full synchronous re-render so the shadow framebuffer in
            // display.c has a complete, up-to-date copy of every pixel.
            lv_obj_invalidate(scr);
            lv_refr_now(lv_display_get_default());
            xSemaphoreGive(s_shot_sem);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void ui_start(void)
{
    s_mtx     = xSemaphoreCreateMutex();
    s_shot_sem = xSemaphoreCreateBinary();
    st.mode = UI_STATS;
    st.nav_level = NAV_SUMMARY;
    strlcpy(st.status, "starting...", sizeof st.status);
    st.dirty = true;
    xTaskCreate(ui_task, "ui", 8192, NULL, 5, NULL);
}

bool ui_capture_screenshot(void)
{
    if (!s_shot_sem) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    st.shot_req = true;
    xSemaphoreGive(s_mtx);
    // Block until the ui_task completes its forced full re-render (≤ ~50 ms).
    return xSemaphoreTake(s_shot_sem, pdMS_TO_TICKS(2000)) == pdTRUE;
}

static void mark(void) { st.dirty = true; }

void ui_set_provisioning(const char *ssid, const char *pass, bool wifi_only)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    st.mode = UI_PROVISION;
    st.nav_level = NAV_SUMMARY;          // leave any open menu/card
    st.prov_wifi_only = wifi_only;
    strlcpy(st.ssid, ssid ? ssid : "", sizeof st.ssid);
    strlcpy(st.pass, pass ? pass : "", sizeof st.pass);
    mark();
    xSemaphoreGive(s_mtx);
}

void ui_set_status(const char *text)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    strlcpy(st.status, text ? text : "", sizeof st.status);
    mark();
    xSemaphoreGive(s_mtx);
}

void ui_set_stats(const stats_t *s, int64_t fetched_uptime_ms)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    st.mode = UI_STATS;
    if (s) { update_provider_activity_locked(s, fetched_uptime_ms); st.stats = *s; stats_model_reorder(&st.stats); }
    st.fetched_ms = fetched_uptime_ms;
    if (!st.boot_complete && st.boot_fade == BOOT_FADE_NONE) {
        int64_t now = esp_timer_get_time() / 1000;
        st.boot_fade = BOOT_FADE_OUT;
        boot_start_fade_locked(config_store_get_brightness(), 0, now);
    }
    if (st.nav_level == NAV_SUMMARY)
        led_summary_reset();
    mark();
    xSemaphoreGive(s_mtx);
}

// LM Studio and Cursor(cu) always present BOTH cards (Today <-> Stats/Limits);
// every other provider only has a Cost card when has_cost. Single-sourced so
// the summary-tap (which card to open) and the page-tap (toggle target) can
// never drift apart.
static bool provider_has_both_cards(const stats_provider_t *p)
{
    provider_kind_t pk = provider_kind(p->id);
    return pk == PK_LMSTUDIO || (pk == PK_CURSOR && p->has_cu);
}

// Card a provider page opens on when tapped from the summary list.
static card_kind_t initial_card_for(const stats_provider_t *p)
{
    if (provider_has_both_cards(p)) return CARD_COST;
    return p->has_cost ? CARD_COST : CARD_LIMITS;
}

// Next card when tapping an already-open provider page (2-card toggle).
static card_kind_t next_card_for(card_kind_t cur, const stats_provider_t *p)
{
    if (provider_has_both_cards(p))
        return (cur == CARD_COST) ? CARD_LIMITS : CARD_COST;
    return (cur == CARD_COST) ? CARD_LIMITS
                              : (p->has_cost ? CARD_COST : CARD_LIMITS);
}

// Navigation state machine (ARCHITECTURE.md decision #9: nav lives in ui.c).
// Runs on the CALLER's task (fetch_task), mutating only `st` under s_mtx —
// exactly like the setters above; no LVGL call here (summary_hit_test /
// clamp_scroll are pure reads/writes of `st` + the cached height).
// Returns UI_INPUT_PASS ONLY for a LONG_PRESS on the summary (→ fetch.c
// opens the add-network portal) or any event in provisioning mode. Every
// other event is consumed by the nav machine.
ui_input_result_t ui_handle_input(const app_evt_t *ev)
{
    if (!ev) return UI_INPUT_PASS;
    ui_input_result_t r = UI_INPUT_CONSUMED;
    xSemaphoreTake(s_mtx, portMAX_DELAY);

    if (st.mode != UI_STATS) {           // provisioning: passthrough
        r = UI_INPUT_PASS;
        goto out;
    }

    st.last_input_ms = esp_timer_get_time() / 1000;
    if (st.saver_active) { saver_exit_locked(st.last_input_ms); led_transition_disable(); r = UI_INPUT_CONSUMED; goto out; }

    if (st.locked) {
        if (ev->type == APP_EVT_SWIPE_UP) {
            st.locked = false;
            st.dirty = true;
        }
        goto out;
    }

    switch (st.nav_level) {
    case NAV_SUMMARY:
        if (ev->type == APP_EVT_SWIPE_UP) {           // page down the list
            st.scroll += summary_vis_rows();
            clamp_scroll();
            st.dirty = true;
        } else if (ev->type == APP_EVT_SWIPE_DOWN) {  // lock the UI
            st.locked = true;
            st.dirty = true;
        } else if (ev->type == APP_EVT_TAP) {
            int pi = summary_hit_test(ev->y);
            if (pi >= 0) {
                const stats_provider_t *tp = &st.stats.p[pi];
                st.nav_provider = pi;
                strlcpy(st.nav_id, tp->id, sizeof st.nav_id);
                st.nav_card = initial_card_for(tp);
                st.nav_level = NAV_PAGE;
                st.dirty = true;
            }
        } else if (ev->type == APP_EVT_LONG_PRESS) {
            r = UI_INPUT_PASS;                         // → enter_portal()
        }
        /* SWIPE_LEFT at the root: swallow (CONSUMED, no-op) */
        break;

    case NAV_PAGE:
        if (ev->type == APP_EVT_TAP) {
            // All providers: 2-card toggle. provider_has_both_cards() decides
            // whether the "no cost data" fallback applies (see next_card_for).
            const stats_provider_t *np = &st.stats.p[st.nav_provider];
            st.nav_card = next_card_for(st.nav_card, np);
            st.dirty = true;
        } else if (ev->type == APP_EVT_SWIPE_DOWN) {   // lock from provider page
            st.locked = true;
            st.dirty = true;
        } else if (ev->type == APP_EVT_SWIPE_LEFT) {   // back to the list
            st.nav_level = NAV_SUMMARY;
            led_summary_reset();
            st.dirty = true;
        }
        /* swipe up/long-press on a page: swallowed (CONSUMED) */
        break;
    }

out:
    xSemaphoreGive(s_mtx);
    return r;
}
