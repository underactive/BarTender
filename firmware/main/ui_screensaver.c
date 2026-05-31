// firmware/main/ui_screensaver.c
//
// Screensaver / boot-splash backlight / idle-dimming logic split out of ui.c.
// All functions here are "_locked": the caller (ui_task / ui_handle_input) holds
// s_mtx, and these mutate the shared `st` and drive display_set_brightness_*.
// No LVGL widget calls live here — only state machine + backlight duty.
#include "ui.h"
#include "ui_internal.h"
#include "config_store.h"
#include "display.h"
#include <string.h>

// ── provider activity tracking (drives which page the saver cycles to) ───────
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

void update_provider_activity_locked(const stats_t *s, int64_t now_ms)
{
    if (!s) return;
    for (int i = 0; i < s->n && i < STATS_MAX_PROVIDERS; i++) {
        const stats_provider_t *p = &s->p[i];
        if (!p->id[0] || is_hidden_provider(p->id) || !p->ok) continue;
        if (!p->has_cost && !provider_has_limits_card(p) && !p->has_lm && !p->has_cu) continue;
        uint32_t sig = provider_metric_sig(p);
        saver_activity_t *slot = activity_slot(p->id);
        if (!slot->has_sig) { slot->sig = sig; slot->has_sig = true; }
        else if (slot->sig != sig) { slot->last_change_ms = now_ms; slot->sig = sig; }
    }
}

int find_provider_id(const char *id)
{
    for (int i = 0; id && i < st.stats.n && i < STATS_MAX_PROVIDERS; i++)
        if (strcmp(st.stats.p[i].id, id) == 0) return i;
    return -1;
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
        if (p->has_cost || p->has_lm || p->has_cu) *card = CARD_COST;
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

void boot_start_fade_locked(uint8_t from, uint8_t to, int64_t now)
{
    st.boot_brightness = from;
    st.boot_target_brightness = to;
    st.boot_fade_start_ms = now;
    st.boot_fade_end_ms = now + BOOT_FADE_MS;
    display_set_brightness_silent(from);
}

void boot_step_fade_locked(int64_t now)
{
    if (st.boot_fade == BOOT_FADE_NONE || st.boot_fade_end_ms <= 0) return;
    if (now >= st.boot_fade_end_ms) {
        display_set_brightness_silent(st.boot_target_brightness);
        st.boot_brightness = st.boot_target_brightness;
        st.boot_fade_end_ms = 0;
        if (st.boot_fade == BOOT_FADE_OUT) {
            st.boot_fade = BOOT_FADE_IN;
            st.dirty = true;
            boot_start_fade_locked(0, config_store_get_brightness(), now);
        } else {
            st.boot_fade = BOOT_FADE_NONE;
            st.boot_complete = true;
        }
        return;
    }
    int64_t dur = st.boot_fade_end_ms - st.boot_fade_start_ms;
    int64_t el = now - st.boot_fade_start_ms;
    int duty = st.boot_brightness +
        (int)(((int)st.boot_target_brightness - (int)st.boot_brightness) * el / dur);
    if (duty < 0) duty = 0;
    else if (duty > 255) duty = 255;
    display_set_brightness_silent((uint8_t)duty);
}

bool boot_splash_visible_locked(void)
{
    return st.mode == UI_STATS && !st.boot_complete && st.boot_fade != BOOT_FADE_IN;
}

void saver_step_fade_locked(int64_t now)
{
    if (st.saver_fade_end_ms > 0) {
        if (now >= st.saver_fade_end_ms) {
            // Fade completed — snap to target and handle page transition.
            display_set_brightness_silent(st.saver_target_brightness);
            st.saver_brightness = st.saver_target_brightness;
            st.saver_fade_end_ms = 0;
            // If a page transition was waiting for fade-to-dim, apply it now.
            if (st.saver_transitioning) {
                if (st.saver_next_show_summary) {
                    st.saver_show_summary = true;
                    st.nav_level = NAV_SUMMARY;
                    st.scroll = 0;
                } else {
                    st.saver_show_summary = false;
                    strlcpy(st.saver_id, st.saver_next_id, sizeof st.saver_id);
                    st.saver_card = st.saver_next_card;
                    st.saver_dim_only = st.saver_next_dim_only;
                    if (!st.saver_dim_only) {
                        int pi = find_provider_id(st.saver_id);
                        if (pi >= 0) {
                            st.nav_provider = pi;
                            strlcpy(st.nav_id, st.saver_id, sizeof st.nav_id);
                            st.nav_card = st.saver_card;
                            st.nav_level = NAV_PAGE;
                        }
                    }
                }
                st.dirty = true;
                st.saver_transitioning = false;
                st.saver_next_cycle_ms = now + SCREENSAVER_PAGE_MS;
                // Fade back up from dim to configured brightness.
                if (!st.saver_dim_only)
                    saver_start_fade_locked(SCREENSAVER_DIM_DUTY, config_store_get_brightness(), now);
            }
        } else {
            int64_t dur = st.saver_fade_end_ms - st.saver_fade_start_ms;
            int64_t el = now - st.saver_fade_start_ms;
            int duty = st.saver_brightness + (int)(((int)st.saver_target_brightness - (int)st.saver_brightness) * el / dur);
            if (duty < 0) duty = 0; else if (duty > 255) duty = 255;
            display_set_brightness_silent((uint8_t)duty);
        }
    }
}

void saver_enter_locked(int64_t now)
{
    if (st.saver_active || st.mode != UI_STATS || st.locked) return;
    st.saved_nav_level = st.nav_level;
    st.saved_nav_provider = st.nav_provider;
    strlcpy(st.saved_nav_id, st.nav_id, sizeof st.saved_nav_id);
    st.saved_nav_card = st.nav_card;
    st.saved_scroll = st.scroll;
    st.saver_active = true;
    st.saver_show_summary = false;
    st.saver_dim_only = !saver_candidate_at(now, 0, st.saver_id, sizeof st.saver_id, &st.saver_card);
    st.saver_next_cycle_ms = now + SCREENSAVER_PAGE_MS;
    saver_start_fade_locked(config_store_get_brightness(), st.saver_dim_only ? SCREENSAVER_DIM_DUTY : config_store_get_brightness(), now);
    if (!st.saver_dim_only) {
        int pi = find_provider_id(st.saver_id);
        if (pi >= 0) { st.nav_level = NAV_PAGE; st.nav_provider = pi; strlcpy(st.nav_id, st.saver_id, sizeof st.nav_id); st.nav_card = st.saver_card; }
    }
    st.dirty = true;
}

void saver_advance_locked(int64_t now)
{
    if (!st.saver_active || st.saver_dim_only || now < st.saver_next_cycle_ms) return;
    if (st.saver_transitioning) return;  // mid-transition, wait for fade to complete

    if (st.saver_show_summary) {
        // Summary was showing — find first active provider to restart the cycle.
        if (!saver_candidate_at(now, 0, st.saver_next_id, sizeof st.saver_next_id, &st.saver_next_card)) {
            st.saver_next_dim_only = true;
        } else {
            st.saver_next_dim_only = false;
        }
        st.saver_next_show_summary = false;
    } else {
        int pi = find_provider_id(st.saver_id);
        if (pi >= 0 && provider_kind(st.stats.p[pi].id) == PK_LMSTUDIO) {
            // LM Studio: 2-card cycle
            int next_card = ((int)st.saver_card + 1) % 2;
            if (next_card == 0 && st.saver_card == CARD_LIMITS) {
                // Wrapped around — check next provider or summary
                int start = 0;
                for (int i = 0; i < STATS_MAX_PROVIDERS; i++)
                    if (st.activity[i].seen && strcmp(st.activity[i].id, st.saver_id) == 0) { start = i + 1; break; }
                if (!saver_candidate_at(now, start, st.saver_next_id, sizeof st.saver_next_id, &st.saver_next_card)) {
                    st.saver_next_dim_only = false;
                    st.saver_next_show_summary = true;
                } else {
                    st.saver_next_dim_only = false;
                    st.saver_next_show_summary = false;
                }
                goto advance_done;
            }
            strlcpy(st.saver_next_id, st.saver_id, sizeof st.saver_next_id);
            st.saver_next_card = (card_kind_t)next_card;
            st.saver_next_dim_only = false;
            st.saver_next_show_summary = false;
        } else if (pi >= 0 && st.saver_card == CARD_COST && provider_card_available(&st.stats.p[pi], CARD_LIMITS)) {
            // Cycle Cost -> Limits on the same provider.
            strlcpy(st.saver_next_id, st.saver_id, sizeof st.saver_next_id);
            st.saver_next_card = CARD_LIMITS;
            st.saver_next_dim_only = false;
            st.saver_next_show_summary = false;
        } else {
            int start = 0;
            for (int i = 0; i < STATS_MAX_PROVIDERS; i++)
                if (st.activity[i].seen && strcmp(st.activity[i].id, st.saver_id) == 0) { start = i + 1; break; }
            if (!saver_candidate_at(now, start, st.saver_next_id, sizeof st.saver_next_id, &st.saver_next_card)) {
                // No more active providers — show summary, then wrap around.
                st.saver_next_dim_only = false;
                st.saver_next_show_summary = true;
            } else {
                st.saver_next_dim_only = false;
                st.saver_next_show_summary = false;
            }
        }
    }
    // Start transition: fade to dim, apply page when fade completes.
advance_done:
    st.saver_transitioning = true;
    saver_start_fade_locked(st.saver_brightness, SCREENSAVER_DIM_DUTY, now);
}

void saver_exit_locked(int64_t now)
{
    if (!st.saver_active) return;
    st.saver_active = false;
    st.saver_dim_only = false;
    st.saver_transitioning = false;
    st.saver_show_summary = false;
    st.saver_next_show_summary = false;
    st.nav_level = st.saved_nav_level;
    st.nav_provider = st.saved_nav_provider;
    strlcpy(st.nav_id, st.saved_nav_id, sizeof st.nav_id);
    st.nav_card = st.saved_nav_card;
    st.scroll = st.saved_scroll;
    saver_start_fade_locked(st.saver_brightness, config_store_get_brightness(), now);
    clamp_scroll();
    st.dirty = true;
}
