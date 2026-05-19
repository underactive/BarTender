# Plan: Scrollable summary + tap-cycle Cost/Limit pages (nav redesign)

- **Started:** 2026-05-18
- **Completed:** 2026-05-18
- **Status:** Completed (build-clean -Werror + 3-reviewer audit; on-device pending user flash)
- **Objective** — Replace the 4-level swipe menu with a scrollable summary
  whose rows are tapped to open per-provider Cost/Limit pages (tap cycles,
  swipe-left backs out), so a provider list longer than the screen is usable.
- **Changes** — app_event.h (+SWIPE_LEFT/LONG_PRESS), touch.c (emit them),
  ui.c/ui.h (2-state nav, windowed-row scroll, delete menu/submenu),
  fetch.c/fetch.h (long-press→portal, drop triple-tap); docs.
- **Dependencies** — app_event.h first; touch.c emits; ui.c consumes;
  fetch.c depends on the new PASS contract; build; docs; audit.
- **Risks / open questions** — touch.c gesture misfire (disjoint bands +
  single-fire gating); nav-index race across stats refresh (s_mtx +
  top-of-render guard + clamp); tap-to-refresh removed (deliberate).

## Context

Summary rows are fixed-position (`ROW_Y0 + i*ROW_H`); only ~5 of up to
`STATS_MAX_PROVIDERS` fit and the rest are silently clipped. Navigation is a
swipe-down menu → submenu → Cost/Limits card. User wants: scroll the summary
by vertical swipe; tap a provider row → its Cost page; tap again → Limit;
tap again → Cost; swipe right→left → back. Locked decisions: re-provision
moves from triple-tap to a long-press on the summary; every provider gets the
Cost↔Limit cycle (non-Claude Cost placeholder already exists at `ui.c:588`).
Full design: `~/.claude/plans/tingly-jingling-brook.md`.

## Steps
- [x] exec-plan doc + PLANS.md row
- [x] app_event.h + touch.c: SWIPE_LEFT + LONG_PRESS
- [x] ui.c/ui.h: 2-state nav, scrollable summary, delete menu code
- [x] fetch.c/fetch.h: route LONG_PRESS→enter_portal, drop triple-tap
- [x] `idf.py -C firmware build` clean (-Werror; 1.50 MB; zero deleted-symbol refs)
- [x] Docs: claude-cost-menu rewrite, esp32-toy, ARCHITECTURE, QUALITY_SCORE,
      READMEs, testing-checklist
- [x] Post-impl: 3-reviewer audit + fixes; sections below filled; moved to
      completed/; PLANS.md updated
- [ ] User on-device verification (8-scenario script) — needs flash

## Files changed

`firmware/main/`: app_event.h (+SWIPE_LEFT/LONG_PRESS, doc), touch.c
(held-branch precedence + SWIPE_LEFT/LONG_PRESS, never-drop-long-press queue
flush, comments), ui.c (2-state machine, st.scroll + st.nav_id,
summary_vis_rows/clamp_scroll/summary_hit_test, windowed render + scroll-aware
`+N more` hint, id-pinned invalidation guard, deleted menu/submenu code,
hide_cards/hide_summary_chrome renames), ui.h (nav/PASS doc), fetch.c
(enter_portal long-press routing, deleted note_tap/triple-tap), fetch.h
(header doc), touch.h (header doc). Docs: claude-cost-menu.md (rewrite),
esp32-toy.md, product-specs/index.md, ARCHITECTURE.md (decision #9 supersede +
#10 + domain row), QUALITY_SCORE.md, README.md, firmware/README.md,
testing-checklist.md, PLANS.md, this exec-plan.

## Implementation summary

Built as planned. 4-level swipe menu → 2-state `{NAV_SUMMARY,NAV_PAGE}`.
Summary scrolls page-step via `st.scroll` windowing the fixed `row_*[]`
objects (no LVGL scroll container); tap a row opens its Cost page; tap cycles
Cost↔Limit; swipe-left returns; long-press → add-network portal. `render_card`
reused unchanged (already had the non-Claude `cost_na` placeholder). ~120
lines of menu/submenu/hit-test code deleted (net simplification). Deviation
from plan: `s_long_fired` dropped — `s_gesture_fired` already provides
one-shot + release-tap suppression, so the extra flag would be set-but-unread
under `-Werror`.

## Audit (3 parallel reviewers, all 7 AUDIT.md personas)

`[FIXED]` resolved this session; unmarked = accepted/refuted.

- `[FIXED]` **QA§P1-2** stale provider identity on refresh-reorder → silent
  page swap. Added `st.nav_id`; render guard now re-resolves the page by id
  (reorder follows, drop-out → summary) instead of by stale index. (ui.c)
- `[FIXED]` **QA§P1-3** `+N more` hint used a global `n-vis`, lying when
  scrolled to the bottom. Now `n - (scroll + vis)` = rows hidden below the
  window. (ui.c)
- `[FIXED]` **R&C§P1-1** touch→fetch queue (depth 8, timeout-0, frozen
  during a blocking fetch) could drop the LONG_PRESS — the only recovery
  gesture. On a full queue the long-press now flushes the queue and
  re-sends (portal intent supersedes queued nav). (touch.c)
- `[FIXED]` **QA§P1-1 (defensive)** added an explicit `idx >=
  STATS_MAX_PROVIDERS` bound in `summary_hit_test` + comment citing the
  `stats_model.c:72` invariant (parser already caps n≤12; this localizes
  the array-safety). (ui.c)
- `[FIXED]` **Interface§P1-2 / P2-3 (docs)** rewrote the stale
  `testing-checklist.md` gesture section; fixed `touch.c`/`touch.h`/`fetch.h`
  comments still naming `APP_EVT_TOUCH`/triple-tap.
- `[FIXED]` **DX§P2** renamed `nav_hide_all`→`hide_cards`,
  `summary_hide`→`hide_summary_chrome` (names were misleading post-redesign).
- *Refuted* — **QA§P1-1 (phantom tap far from lift)**: the release-TAP path
  already rejects via `|s_last−s_press| ≤ TAP_SLOP` AND `≤ TAP_MAX_US`; the
  12–40 px inert zone is documented intended behavior, not a regression.
  **QA§P1-4 (provisioning PASS "dead")**: in provisioning mode `fetch_task`
  is never started (main.c only starts it in the STA branch), so the
  blanket-PASS branch is defensive/unreached from that path — comment lightly
  clarified, no code change. Lock-free/s_mtx discipline, "no LVGL off
  ui_task", and "no dangling deleted-symbol refs" were all **confirmed** by
  the reviewers. Host-test gap for the pure geometry helpers
  (`summary_vis_rows`/`clamp_scroll`/`summary_hit_test`) is pre-existing
  no-harness debt (already tracked) — added to follow-ups.

### Audit Fixes — verification checklist
- [ ] Open provider X's page; republish with providers REORDERED (same
      count) → page still shows X (not whatever slid into the old index).
- [ ] Open X's page; republish WITHOUT X → falls back to the summary.
- [ ] Scroll to the bottom of a long list → `+N more` shows 0 / disappears;
      at the top it shows the true hidden-below count.
- [ ] Spam taps/swipes during a slow fetch, then long-press → portal still
      opens (queue-flush path).
- [ ] Quick tap never opens the portal; 3 fast taps just cycle pages.

## Verification

`idf.py -C firmware build` clean (1.50 MB, 82% free), `-Werror`, zero
warnings (the `-Werror=format-truncation` on the new `hint` buffer was caught
and fixed by sizing; pre-existing unrelated `kconfig LCD_HOST_SPI2` warnings
are not from this change). Reviewers confirmed: exactly one gesture per
press; `nav_provider` can't index OOB (parser caps n≤12 + guard + explicit
bound); all `st` access under `s_mtx`; no LVGL off `ui_task`; no dangling
reference to any deleted menu/triple-tap symbol anywhere in `firmware/main`.
No host harness exists → on-device run is the 8-scenario script below + the
Audit-Fixes checklist, handed to the user (not flashed this session).

## Follow-ups

- On-device run of the 8-scenario + audit-fix checklists (user; needs flash).
- Host-test seam for `summary_vis_rows`/`clamp_scroll`/`summary_hit_test`
  (pure, zero-LVGL) — same `firmware/test/host/` debt already tracked.
- Long-press on a *page* is currently free — reserved for a possible future
  manual refresh (tap-to-refresh was removed by this redesign).
- Optional: lower `DEBOUNCE_US` (200 ms) now that the triple-tap timing
  dependency is gone, for snappier Cost↔Limit double-taps (low priority).

## Decisions
- 2026-05-18: 2-state nav `{SUMMARY,PAGE}` (was 4-level menu) — simplest model
  matching the tap-cycle UX.
- 2026-05-18: Window the existing fixed `row_*[ROWS]` objects via an integer
  `st.scroll` offset (no LVGL scroll container) — touch is a custom
  poll→event pipeline, not an LVGL indev; keeps all LVGL on ui_task.
- 2026-05-18: Page-step scroll (swipe events are discrete, no delta).
- 2026-05-18: Long-press (~1.5 s) on summary supersedes triple-tap for
  re-provision (user decision); triple-tap detector deleted.
- 2026-05-18: Reuse `render_card()` unchanged for NAV_PAGE — it already
  handles the non-Claude Cost placeholder; tap-to-refresh removed as a
  deliberate consequence (300 s poll keeps pages live).

## Open questions
- (resolved via AskUserQuestion — none outstanding)

## Files changed
_(filled at completion)_

## Implementation summary
_(filled at completion)_

## Verification
_(filled at completion)_

## Follow-ups
_(filled at completion)_
