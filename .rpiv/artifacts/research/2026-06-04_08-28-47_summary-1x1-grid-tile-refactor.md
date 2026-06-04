---
date: 2026-06-04T08:28:47-0700
author: Eric Sison
commit: 72ea1da
branch: main
repository: bartender
topic: "summary page 1x1 grid tile refactor"
tags: [research, codebase, firmware, ui, summary-grid]
status: complete
last_updated: 2026-06-04T08:28:47-0700
last_updated_by: Eric Sison
---

# Research: Summary Page 1×1 Grid Tile Refactor

## Research Question
Currently the summary page scrolls a list of widgets 2 columns wide by 1 row high. Instead of scrolling, make new smaller widgets for the summary that are 1 col x 1 row so more providers can be seen on the summary page without scrolling. The smaller widgets should have the icon, smaller progressbar(s) and the percentage. The provider names can get long, so don't display those in the 1×1 widget. Instead show the percentage where the provider name would have been.

## Summary
The summary page currently uses an auto-scrolling ticker (`AUTO_SCROLL_DELTA=1.2px/frame` at `ui_internal.h:40`) to march 2-col×1-row widgets through a viewport, with each row consuming `ROW_H=39px` and showing `icon + name + primary% + secondary_bar`. The 2×8 grid model (`UI_GRID_COLS=2`, `UI_GRID_ROWS=8`) is already implemented via `ui_grid_from_height()` / `ui_grid_span()` in `ui_format.c:10-30`. The summary hero area (I/O TOKENS) already reserves the top `UI_SUMMARY_TOP_ROWS=2` grid rows via `ui_grid_span(&g, 0, 0, 2, 2)` at `ui_render_core.c:358`.

Switching to a 1×1 grid requires replacing the ticker with a fixed grid fill (col=0..1, row=2..7), removing the auto-scroll mechanics (`st.auto_scroll_px`, `row_shift`/`pixel_shift`), rewriting `summary_hit_test()` to use grid cell arithmetic, and creating a new `render_grid_tile()` function that positions all five widgets (`row_icon`, `row_id`, `row_bar`, `row_val`, `row_bar_w`) relative to a `ui_rect_t` cell rather than a pixel y-offset. The widget arrays (`ROWS=STATS_MAX_PROVIDERS=12`) exactly fill the 2×6 grid (12 cells) below the hero area — no scrolling needed.

## Detailed Findings

### Auto-Scrolling Ticker — What to Remove

The ticker mechanism spans three files:

**Accumulation** (`ui.c:143`): `st.auto_scroll_px += AUTO_SCROLL_DELTA` runs every frame in `ui_task()` when `st.nav_level == NAV_SUMMARY`. This is the sole writer.

**Clamp + Decompose** (`ui_render_core.c:1025-1028`): `while (st.auto_scroll_px >= (float)total_h) st.auto_scroll_px -= (float)total_h` wraps the offset, then `row_shift = (int)(st.auto_scroll_px / ROW_H) % count` and `pixel_shift = (int)st.auto_scroll_px % ROW_H` derive the visible row offset and sub-pixel position.

**Render loop** (`ui_render_core.c:1031-1048`): All 12 widget slots are hidden, then `rows_to_draw = (viewport_h + ROW_H - 1) / ROW_H + 2` rows are rendered at `y = content_y0 + slot * ROW_H - pixel_shift`.

**Hit-test mirror** (`ui.c:73-82`): `summary_hit_test()` duplicates the same `scroll_px` → `row_shift`/`pixel_shift` → slot math.

All four loci must be removed or replaced. The `auto_scroll_px` field (`ui_internal.h:171`), `AUTO_SCROLL_DELTA` constant (`ui_internal.h:40`), and `ROW_H` constant (`ui_internal.h:27`) become dead code.

### Grid Fill Order + Hidden-Provider Compaction

**Compaction layer** (`ui.c:35-52`): `summary_visible_count()` iterates `st.stats.p[]` skipping hidden providers (`"ollama"`, `"opencode"` per `HIDDEN_PROVIDERS` at `ui_internal.h:63`). `summary_provider_at(visible_idx)` maps a compacted visible index back to the raw `stats.p[]` index by walking the array and skipping hidden entries.

**Grid layout** (`ui_format.c:10-20`): On 240×320 screen: `cell_w=120`, `cell_h=35`. The 6 rows (2..7) × 2 cols = 12 cells exactly match `STATS_MAX_PROVIDERS=12` (`stats_model.h:13`).

**Proposed render loop shape** — row-major fill, slot n increments sequentially:
```
int slot = 0;
for (int row = UI_SUMMARY_TOP_ROWS; row < UI_GRID_ROWS; row++) {
    for (int col = 0; col < UI_GRID_COLS; col++) {
        int pi = summary_provider_at(slot);
        if (pi >= 0) {
            ui_rect_t cell = ui_grid_span(&g, col, row, 1, 1);
            render_grid_tile(slot, &st.stats.p[pi], &cell);
        }
        slot++;
    }
}
```
When `pi < 0` (fewer visible providers than 12), the widget slot stays hidden — no empty tile renders. The pre-existing hide-all loop at `ui_render_core.c:1031-1040` already establishes this invariant: all slots start hidden, and `render_grid_tile()` only clears HIDDEN flags for its widgets.

### Widget Layout Within a 1×1 Tile

**Current row layout** — `render_summary_row()` at `ui_render_summary.c:27-73`:
- `row_icon[slot]`: 32×32 at `(8, pixel_y + 3)`
- `row_id[slot]`: name text at `(48, pixel_y + 2)`, width `W - 56`
- `row_bar[slot]`: primary bar at `(48, pixel_y + 21)`, size `(W - 108, 5)`
- `row_val[slot]`: percentage at `(W - 52, pixel_y + 15)`
- `row_bar_w[slot]`: secondary bar at `(48, pixel_y + 28)`, size `(W - 108, 2)`

**New tile layout** — `render_grid_tile()` replaces the above:
- `row_icon[slot]`: 24×24 at `(cell->x + 4, cell->y + (cell->h - 24)/2)` — centered vertically
- `row_id[slot]` text: `fmt_pct()` output (percentage) instead of provider name, at `(cell->x + 32, cell->y + 2)`, width `cell->w - 36`
- `row_bar[slot]`: primary bar at `(cell->x + 32, cell->y + 18)`, size `(cell->w - 36, 5)`
- `row_val[slot]`: **hidden** (percentage now in `row_id` — no redundancy)
- `row_bar_w[slot]`: secondary bar at `(cell->x + 32, cell->y + 25)`, size `(cell->w - 36, 2)`

**Secondary bar logic** — `render_summary_secondary_bar()` at `ui_render_summary.c:10-24` is unchanged: Claude/Codex weekly %, LM Studio requests %, OpenRouter budget % — the logic is position-independent.

### Summary Hero Area (Unchanged)

The hero area spans `ui_grid_span(&g, 0, 0, 2, 2)` at rows 0-1 (`ui_render_core.c:358`). `place_summary_hero_amount(&cost_hero, &top_r, "I/O TOKENS")` at `ui_render_core.c:359-363` uses the grid span for positioning. The `summary_tok_today_total()` function (`ui.c:82-90`) sums today's tokens, and `fmt_tokens_full()` renders the number. None of this changes.

The `cost_hero` reparenting between `scr` (summary) and `cost.card` (card page), handled by `cost_hero_set_parent()` at `ui_render_core.c:222` and `ui_render_core.c:356`, is orthogonal to the grid refactor.

### Card-Transition Animation Triggers (Unchanged)

When a user taps a grid tile, `handle_summary_event()` at `ui.c:283-301` sets `st.nav_provider`, `st.nav_id`, `st.nav_card`, `st.nav_level = NAV_PAGE`. `render()` at `ui_render_core.c:890-897` re-resolves `st.nav_id` against current `stats.p[]` (safety net for provider reordering). `render_card()` at `ui_render_card.c:649-658` detects the entry by comparing `st.prev_nav_level != NAV_PAGE` and triggers `anim_count_up()` / `anim_chart_fadein()`.

The only impact on the card chain is that `summary_hit_test()` must return the correct `stats.p[]` index so `st.nav_provider` and `st.nav_id` are correctly set.

## Code References

- `ui_internal.h:27` — `ROW_H 39` (will become dead code after refactor)
- `ui_internal.h:28` — `ROW_ICON_PX 32` (icon shrinks to 24)
- `ui_internal.h:40` — `AUTO_SCROLL_DELTA 1.2f` (will become dead code)
- `ui_internal.h:63` — `HIDDEN_PROVIDERS` macro
- `ui_internal.h:66-72` — `UI_GRID_COLS`, `UI_GRID_ROWS`, `UI_CHROME_TOP`, `UI_CHROME_BOTTOM`, `UI_SUMMARY_TOP_ROWS`
- `ui_internal.h:171` — `float auto_scroll_px` (field to remove)
- `ui_internal.h:257` — `int summary_hit_test(int y)` declaration
- `ui_internal.h:311` — `void render_summary_row(...)` declaration
- `ui_format.c:10-20` — `ui_grid_from_height()` computes grid geometry
- `ui_format.c:22-30` — `ui_grid_span()` returns cell rects
- `ui_format.c:297-303` — `fmt_pct()` formats percentage string used in new tile
- `ui.c:35-41` — `summary_visible_count()` — compaction function
- `ui.c:44-52` — `summary_provider_at()` — visible→raw index mapping
- `ui.c:61-82` — `summary_hit_test()` — needs `x` param + grid cell arithmetic
- `ui.c:143` — `auto_scroll_px` increment in `ui_task()` — to remove
- `ui.c:283-301` — `handle_summary_event()` — call site for hit-test
- `ui_render_core.c:70-135` — `build_summary_widgets()` — widget creation
- `ui_render_core.c:358-363` — hero area placement (unchanged)
- `ui_render_core.c:1025-1048` — ticker render loop in `render()` — to replace
- `ui_render_summary.c:27-73` — `render_summary_row()` — to become `render_grid_tile()`
- `ui_render_summary.c:10-24` — `render_summary_secondary_bar()` — unchanged logic
- `stats_model.h:13` — `STATS_MAX_PROVIDERS 12`
- `stats_model.c:355-382` — `stats_model_reorder()` — display order for provider tiles

## Integration Points

### Inbound References
- `ui.c:291` — `handle_summary_event()` calls `summary_hit_test()` (only call site)
- `ui_render_core.c:1047` — `render()` calls `render_summary_row()` (only call site)
- `ui.c:143` — `ui_task()` increments `auto_scroll_px` (only writer)

### Outbound Dependencies
- `ui_format.c:10-30` — grid geometry helpers used by both render and hit-test
- `ui_format.c:297-303` — `fmt_pct()` for percentage label text
- `ui_format.c:227-239` — `summary_provider_name()` (will become unused for tiles)
- `ui_internal.h` — all shared constants and declarations

### Infrastructure Wiring
- `ui_internal.h:40` — `AUTO_SCROLL_DELTA` constant (dead after refactor)
- `ui_internal.h:171` — `auto_scroll_px` field in `struct ui_state` (dead after refactor)
- `ui_internal.h:27` — `ROW_H` constant (dead after refactor)

## Architecture Insights

1. **Render + hit-test must be updated atomically.** Both derive row geometry from the same ticker math. Every prior layout change (touch-scroll → auto-ticker, grid refactor) has required simultaneous updates to both. The 1×1 grid replaces the shared math with `ui_grid_span()` cell boundaries.

2. **The 2×8 grid infrastructure is already in place** from `2f89cb0` but the auto-scrolling ticker bypasses it for summary rows, using `ROW_H` + pixel offsets instead. Refactoring to 1×1 tiles aligns the summary with the grid model that card pages already use.

3. **`summary_hit_test()` signature changes from `int y` to `int x, int y`.** The `app_evt_t` struct already carries both coordinates (`app_event.h:24`). Only `handle_summary_event()` at `ui.c:291` calls this function. The declaration at `ui_internal.h:257` and definition at `ui.c:61` need updating.

4. **Hidden-provider compaction survives any layout change.** `summary_visible_count()` and `summary_provider_at()` bridge visible slots to `stats.p[]` — this mapping is used by render, hit-test, and the hero-total function. The grid does not alter this layer.

5. **No LVGL calls off the UI task.** Hit-test must use cached `s_scr_w` / `s_scr_h` (set in `build_widgets()`), not `lv_display_get_*()` calls, because it runs on the fetch/input task.

## Precedents & Lessons

5 similar past changes analyzed.

### Precedent: Auto-scrolling ticker replacing touch-scroll
**Commit(s)**: `a8de104` — "fix(ui): smooth summary ticker and prevent row bleed" (2026-06-02)
**Blast radius**: 5 files across 2 layers
  firmware/main/ui.c — replaced `st.scroll` + user swipe with `st.auto_scroll_px` + frame-tick increment
  firmware/main/ui_render.c — added `footer_bg` backdrop, tightened row Y positions
  firmware/main/ui_internal.h — added `AUTO_SCROLL_DELTA`, `auto_scroll_px`
**Follow-up fixes**:
  - `58029c8` — extracted into `ui_render_core.c` / `ui_render_summary.c` same day
  - `a0d02b3` — stopped bar-pulse/cursor anims on off-screen rows
**Takeaway**: The ticker added a continuous render-on-every-frame requirement that a static 1×1 grid eliminates.

### Precedent: Display widget-grid refactor — shared 2×8 grid foundation
**Commit(s)**: `2f89cb0` — "fix(ui): refactor display layout to shared grid" (2026-05-29)
**Blast radius**: 5 files across 2 layers — introduced `ui_grid_from_height()`, `ui_grid_span()`, `ui_rect_t`, `ui_page_grid_t`
**Follow-up fixes**: 4 fixes same day (`57de194`, `c20e777`, `2abbac4`, plus prior overlap fixes)
**Takeaway**: Grid geometry must stay pure-math (no LVGL calls) when called from the input path.

### Precedent: Summary-row extraction (Fowler audit #5)
**Commit(s)**: `855985b` — "refactor(ui): extract tier-row + summary-row helpers" (2026-05-31)
**Blast radius**: 8 files across 3 layers — extracted `render_summary_row()`, added `test/ui_format/` host tests
**Takeaway**: `render_summary_row()` currently accepts `pixel_y` for ticker positioning; a 1×1 grid replaces this with cell-slot parameters.

### Precedent: Scrollable summary + nav redesign
**Commit(s)**: `3681e55` — "feat: scrollable summary + tap-cycle Cost/Limit pages" (2026-05-18)
**Blast radius**: 17 files across 4 layers — 2-state nav machine, windowed row rendering, `summary_hit_test()`
**Takeaway**: Navigation state and summary layout are tightly coupled.

### Precedent: Grid layout polish — highest bug density
**Commit(s)**: `57de194` — "feat(ui): polish grid layout" (2026-05-29), 1152 insertions / 933 deletions
**Follow-up fixes**: 4 same-day fixes for overlap/positioning regressions
**Takeaway**: Expect 2-3 positioning regressions; budget for screenshot-backed validation on every provider configuration.

### Composite Lessons
- Render + hit-test must be updated atomically — every layout change has required both. Use `ui_grid_from_height()` / `ui_grid_span()` as the single geometry source that both consume.
- Grid/layout refactors produce the highest regression density in the project. Expect at least 2-3 positioning regressions and validate with screenshots.
- The auto-scrolling ticker adds continuous-frame rendering that a static grid eliminates, but the row geometry contract is deeply embedded across 3 files.
- Hidden-provider compaction survives any layout change — `summary_visible_count()` / `summary_provider_at()` remain.
- No LVGL calls off the UI task — hit-test must use cached screen dimensions.

## Historical Context (from `.rpiv/artifacts/`)
- `.rpiv/artifacts/research/2026-05-28_23-27-26_display-widget-grid-refactor.md` — Research summary for the original 2×8 grid refactor
- `.rpiv/artifacts/designs/2026-05-28_23-42-45_display-widget-grid-refactor.md` — Design artifact for the 2×8 grid
- `.rpiv/artifacts/plans/2026-05-28_23-54-57_display-widget-grid-refactor.md` — Implementation plan for grid refactor Phases 1+2
- `.rpiv/artifacts/discover/2026-05-28_23-20-45_display-widget-grid-refactor.md` — Discovery for the original grid refactor

## Developer Context
**Q (`ui.c:61`): `summary_hit_test()` currently takes `int y` but the 2-column grid needs both X and Y. Should the signature change to `int summary_hit_test(int x, int y)`?**
A: Yes, add the `x` param. This is the recommended approach. The `app_evt_t` struct already carries both `.x` and `.y`. Only `handle_summary_event()` at `ui.c:291` calls this function, so the change is contained.

## Related Research
- `.rpiv/artifacts/research/2026-05-28_23-27-26_display-widget-grid-refactor.md` — Original 2×8 grid refactor research (precedent for grid infrastructure)
- `docs/exec-plans/completed/fowler-audit-refactors.md` — Fowler audit that extracted `render_summary_row()`

## Open Questions
None — all questions resolved during checkpoint.
