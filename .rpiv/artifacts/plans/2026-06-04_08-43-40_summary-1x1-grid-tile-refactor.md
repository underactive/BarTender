---
date: 2026-06-04T08:43:40-0700
author: Eric Sison
commit: 72ea1da
branch: main
repository: bartender
topic: "Summary Page 1x1 Grid Tile Refactor"
tags: [plan, firmware, ui, summary-grid]
status: ready
parent: .rpiv/artifacts/research/2026-06-04_08-28-47_summary-1x1-grid-tile-refactor.md
phase_count: 4
unresolved_phase_count: 0
last_updated: 2026-06-04T08:43:40-0700
last_updated_by: Eric Sison
---

# Summary Page 1×1 Grid Tile Refactor — Implementation Plan

## Overview
Replace the auto-scrolling ticker on the summary page with a static 2×6 grid of compact 1×1 tiles. Each tile shows a 24×24 provider icon, a percentage label (where the provider name used to be), a primary usage bar, and an optional secondary bar — all positioned relative to a grid cell rect via `ui_grid_span()`. The hero area (I/O TOKENS, rows 0-1) is unchanged. The existing `row_*[ROWS]` widget arrays are reused with new per-frame positioning.

## Requirements
- Remove auto-scrolling ticker behavior from the summary page
- Render providers in a fixed 2-column grid (rows 2..7 × cols 0..1 = 12 cells)
- Each 1×1 tile shows: 24×24 icon, percentage label (where name was), primary progress bar, secondary bar (where applicable)
- Provider name is NOT displayed in the tile (saves space)
- Tapping a tile navigates to the provider's card page
- Hero area (I/O TOKENS, rows 0-1) is unchanged
- Hidden providers (ollama, opencode) remain hidden
- Empty cells (fewer than 12 visible providers) render nothing
- `AUTO_SCROLL_DELTA` constant and `auto_scroll_px` field remain but are no longer incremented

## Current State Analysis
The summary page uses a continuous auto-scrolling ticker:
- `st.auto_scroll_px += AUTO_SCROLL_DELTA` (1.2px/frame) in `ui_task()` at `ui.c:143`
- Clamp + decompose into `row_shift`/`pixel_shift` at `ui_render_core.c:1025-1028`
- Render loop hides all 12 slots, then renders `rows_to_draw` rows at pixel-level y-offsets (`ui_render_core.c:1031-1048`)
- `render_summary_row(slot, p, pixel_y, W)` positions widgets at absolute pixel positions (`ui_render_summary.c:27-73`)
- `summary_hit_test(int y)` mirrors the ticker math to map touch → provider index (`ui.c:61-82`)
- `handle_summary_event()` calls `summary_hit_test(ev->y)` at `ui.c:291`

The 2×8 grid infrastructure already exists:
- `ui_grid_from_height()` / `ui_grid_span()` at `ui_format.c:10-30`
- `UI_GRID_COLS=2`, `UI_GRID_ROWS=8`, `UI_SUMMARY_TOP_ROWS=2` at `ui_internal.h:66-72`
- Card page renderers already use `ui_grid_span()` for positioning (`ui_render_card.c`)

### Key Discoveries
- `render_summary_row()` at `ui_render_summary.c:27-73` — the function to replace with `render_grid_tile()`
- `render_summary_secondary_bar()` at `ui_render_summary.c:10-24` — position-independent logic, unchanged
- `summary_hit_test()` at `ui.c:61-82` — must be rewritten with grid cell arithmetic + `x` param
- `build_summary_widgets()` at `ui_render_core.c:70-135` — widget creation, unchanged
- `ui_internal.h:27` — `ROW_H` 39 (becomes unused but stays)
- `ui_internal.h:40` — `AUTO_SCROLL_DELTA` 1.2f (becomes unused but stays)
- `ui_internal.h:171` — `float auto_scroll_px` (field stays, no longer incremented)
- `place_summary_hero_amount()` at `ui_render_core.c:313-319` — pattern for cell-relative positioning

## Desired End State
After implementation, the summary page renders a fixed grid of 1×1 tiles:

```c
// Grid render loop (in render() NAV_SUMMARY branch):
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

// Tile renderer positions widgets relative to cell rect:
// row_icon[slot]: 24×24 at (cell->x + 4, cell->y + (cell->h - 24)/2)
// row_id[slot]: fmt_pct() at (cell->x + 32, cell->y + 2), width cell->w - 36
// row_bar[slot]: primary at (cell->x + 32, cell->y + 18), size (cell->w-36, 5)
// row_val[slot]: hidden
// row_bar_w[slot]: secondary at (cell->x + 32, cell->y + 25), size (cell->w-36, 2)

// Hit-test uses grid cell arithmetic:
// col = (ev->x - g.content.x) / g.cell_w
// row = (ev->y - content_y0) / g.cell_h
// visible_idx = row * UI_GRID_COLS + col
```

## What We're NOT Doing
- Not creating a new `tile_widgets_t` struct — reusing existing `row_*[ROWS]` arrays
- Not creating a new `ui_render_tile.c` — keeping code in `ui_render_summary.c`
- Not removing `AUTO_SCROLL_DELTA` or `auto_scroll_px` — leaving dead but harmless
- Not changing the hero area (I/O TOKENS, rows 0-1)
- Not changing card pages or their transition animations
- Not adding provider names back into tiles
- Not modifying `build_summary_widgets()` widget creation
- Not changing the screenshot protocol, LED behavior, or screensaver

## Decisions

### Reuse existing row_* widget arrays
Rather than creating a new `tile_widgets_t[ROWS]` struct, the existing `row_icon[ROWS]`, `row_id[ROWS]`, `row_bar[ROWS]`, `row_val[ROWS]`, and `row_bar_w[ROWS]` arrays are reused. Every frame, `render_grid_tile()` repositions them relative to the cell rect via `lv_obj_set_pos()`. This minimizes the diff and avoids creating new widget instances.

### render_grid_tile() lives in ui_render_summary.c
Keeps all summary-row/tile rendering in one file alongside the existing `render_summary_secondary_bar()`. No new translation unit needed, no CMakeLists.txt update.

### Leave dead constants in place
`AUTO_SCROLL_DELTA` (`ui_internal.h:40`) and `auto_scroll_px` (`ui_internal.h:171`) remain in the source. Only the increment at `ui.c:143` is removed. Safe, minimal change.

### summary_hit_test() gets an `x` param
The signature changes from `summary_hit_test(int y)` to `summary_hit_test(int x, int y)`. The `app_evt_t` struct already carries both coordinates (`app_event.h:24`). The sole call site at `ui.c:291` is updated.

## Phase 1: Declarations

### Overview
Update `ui_internal.h` with the new function declarations for `render_grid_tile()` and updated `summary_hit_test()`.

### Changes Required:

#### 1. firmware/main/ui_internal.h:311
**File**: firmware/main/ui_internal.h
**Changes**: MODIFY — Add `render_grid_tile()` declaration alongside existing `render_summary_row()` declaration (the old function is still defined and called until Phases 3-4 remove its references)

```c
// Keep the existing render_summary_row declaration at line 311:
// void render_summary_row(int slot, const stats_provider_t *p, int pixel_y, int W);
//
// Add the new grid tile declaration immediately after:
void render_grid_tile(int slot, const stats_provider_t *p, const ui_rect_t *cell);
```

#### 2. (moved to Phase 4 — updated alongside definition)
**File**: firmware/main/ui_internal.h
**Changes**: NONE in Phase 1. The `summary_hit_test` declaration at `ui_internal.h:257` stays as `int summary_hit_test(int y)` in this phase. Both the declaration AND definition change together in Phase 4 to avoid a broken intermediate state.

### Success Criteria:

#### Automated Verification:
- [ ] Declaration-only consistency check: `grep -n "render_grid_tile" firmware/main/ui_internal.h` shows the new declaration

#### Manual Verification:
- [ ] Declarations match the function signatures used in later phases

## Phase 2: Tile Renderer

### Overview
Replace `render_summary_row()` in `ui_render_summary.c` with new `render_grid_tile()` that positions widgets relative to a `ui_rect_t` cell. Keep `render_summary_secondary_bar()` unchanged.

### Changes Required:

#### 3. firmware/main/ui_render_summary.c:27-105
**File**: firmware/main/ui_render_summary.c
**Changes**: MODIFY — ADD `render_grid_tile()` alongside existing `render_summary_row()` (old function stays until Phase 3 removes its call site). Positions widgets relative to `cell->x`/`cell->y`.

```c
// Render one 1x1 grid tile: icon + percentage + primary bar + secondary bar.
// `slot` indexes into the row_*[ROWS] widget arrays (0..ROWS-1).
// `cell` is the precomputed grid cell rect from ui_grid_span().
// Provider name is NOT shown — the percentage label takes its place.
void render_grid_tile(int slot, const stats_provider_t *p,
                      const ui_rect_t *cell)
{
    lv_obj_clear_flag(row_id[slot],  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(row_val[slot], LV_OBJ_FLAG_HIDDEN);  // percentage now in row_id

    // Compact icon: 24x24, left-aligned in cell, vertically centered
    lv_obj_set_size(row_icon[slot], 24, 24);
    lv_obj_set_pos(row_icon[slot], cell->x + 4,
                   cell->y + (cell->h - 24) / 2);

    // Percentage label where the provider name used to be
    char pctbuf[12];
    if (p->ok && p->primary.has) {
        fmt_pct(pctbuf, sizeof pctbuf, true, p->primary.pct);
    } else {
        snprintf(pctbuf, sizeof pctbuf, "--");
    }
    lv_label_set_text(row_id[slot], pctbuf);
    lv_obj_set_style_text_color(row_id[slot], lv_color_hex(0xe8eaed), 0);
    lv_obj_set_pos(row_id[slot], cell->x + 32, cell->y + 2);
    lv_obj_set_width(row_id[slot], cell->w - 36);

    // Primary bar
    lv_obj_set_pos(row_bar[slot], cell->x + 32, cell->y + 18);
    lv_obj_set_size(row_bar[slot], cell->w - 36, 5);

    // Secondary bar
    lv_obj_set_pos(row_bar_w[slot], cell->x + 32, cell->y + 25);
    lv_obj_set_size(row_bar_w[slot], cell->w - 36, 2);

    // Provider logo: A8 silhouette (tinted via recolor) or ARGB8888
    // full-color image (no tinting). Hidden if no icon for this id.
    const lv_image_dsc_t *ic = provider_summary_icon(p->id);
    if (ic) {
        lv_image_set_src(row_icon[slot], ic);
        if (provider_icon_is_full_color(p->id)) {
            lv_obj_set_style_image_recolor_opa(
                row_icon[slot], LV_OPA_TRANSP, 0);
        } else {
            lv_color_t tc;
            lv_obj_set_style_image_recolor_opa(
                row_icon[slot], LV_OPA_COVER, 0);
            lv_obj_set_style_image_recolor(row_icon[slot],
                prov_accent(p->id, &tc) ? tc : lv_color_hex(0xe8eaed), 0);
        }
        lv_obj_clear_flag(row_icon[slot], LV_OBJ_FLAG_HIDDEN);
        update_cursor_sess_pulse(row_icon[slot], cursor_sess_refresh_needed(p));
    } else {
        lv_obj_add_flag(row_icon[slot], LV_OBJ_FLAG_HIDDEN);
        update_cursor_sess_pulse(row_icon[slot], false);
    }

    if (!p->ok || !p->primary.has) {
        update_bar_pulse(row_bar[slot], 0.0f);
        lv_obj_add_flag(row_bar[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(row_id[slot], lv_color_hex(0x6b7075), 0);
    } else {
        int v = clampi((int)(p->primary.pct + 0.5f), 0, 100);
        int fill = bar_fill(v);
        lv_obj_clear_flag(row_bar[slot], LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(row_bar[slot], fill, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(row_bar[slot], bar_color(p, p->primary.pct), LV_PART_INDICATOR);
        update_bar_pulse(row_bar[slot], p->primary.pct);
        lv_obj_set_style_text_color(row_id[slot], lv_color_hex(0xffffff), 0);
        render_summary_secondary_bar(slot, p);
    }
}
```

### Success Criteria:

#### Automated Verification:
- [ ] Build passes: `cd firmware && idf.py build`
- [ ] Existing ui_format tests pass: `cd firmware/test/ui_format && ./runtests`

#### Manual Verification:
- [ ] Summary page shows 1×1 tiles with icon, percentage, and bars (no provider names)
- [ ] Icons render at 24×24 with proper tinting for each provider
- [ ] Percentage text shows "XX.X%" format with proper coloring (white for active, gray for "off")
- [ ] Primary bar renders at correct width and color
- [ ] Secondary bar (weekly/budget) renders correctly for applicable providers
- [ ] "Offline" providers show "--" percentage and hidden bar

## Phase 3: Grid Render Loop

### Overview
Replace the auto-scrolling ticker render loop in `render()` with a fixed grid fill that iterates rows 2..7 × cols 0..1.

### Changes Required:

#### 4. firmware/main/ui_render_core.c:1025-1048
**File**: firmware/main/ui_render_core.c
**Changes**: MODIFY — Replace the ticker clamp/decompose/render loop with a fixed grid fill loop. Keep the hide-all widgets preamble and the `content_y0`/`footer_slot` computation.

Replace this block (from `while (st.auto_scroll_px >= (float)total_h)` through the end of the for loop):

```c
    // ── Grid tile render: fixed 2×6 grid below hero area ──
    int slot = 0;
    for (int row = UI_SUMMARY_TOP_ROWS; row < UI_GRID_ROWS; row++) {
        for (int col = 0; col < UI_GRID_COLS; col++) {
            int pi = summary_provider_at(slot);
            if (pi >= 0) {
                const ui_rect_t cell = ui_grid_span(&g, col, row, 1, 1);
                render_grid_tile(slot, &st.stats.p[pi], &cell);
            }
            slot++;
        }
    }
```

And remove these lines above it that become dead:
```c
    int count = summary_visible_count();
    int total_h = count * ROW_H;
    if (count <= 0 || total_h <= 0) {
```
...the hide-all-loop (lines 1031-1040) stays — it runs before the grid loop.

The full replacement context is:

```c
    render_lock_badge(st.locked, s_scr_w - 18, 4);
    int count = summary_visible_count();
    // ── Grid tile render: fixed 2×6 grid below hero area ──
    // Hide all widget slots first, then show only populated tiles
    for (int slot = 0; slot < ROWS; slot++) {
        update_bar_pulse(row_bar[slot],   0.0f);
        update_bar_pulse(row_bar_w[slot], 0.0f);
        update_cursor_sess_pulse(row_icon[slot], false);
        lv_obj_add_flag(row_id[slot],   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_bar[slot],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_val[slot],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_icon[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_bar_w[slot], LV_OBJ_FLAG_HIDDEN);
    }
    if (count > 0) {
        int slot = 0;
        for (int row = UI_SUMMARY_TOP_ROWS; row < UI_GRID_ROWS; row++) {
            for (int col = 0; col < UI_GRID_COLS; col++) {
                int pi = summary_provider_at(slot);
                if (pi >= 0) {
                    const ui_rect_t cell = ui_grid_span(&g, col, row, 1, 1);
                    render_grid_tile(slot, &st.stats.p[pi], &cell);
                }
                slot++;
            }
        }
    }
```

### Success Criteria:

#### Automated Verification:
- [ ] Build passes: `cd firmware && idf.py build`

#### Manual Verification:
- [ ] No auto-scrolling — tiles are stationary
- [ ] Grid fills left-to-right, top-to-bottom (row-major)
- [ ] All 12 cells available, visible providers fill them in order
- [ ] Empty cells (fewer than 12 visible providers) show no ghost widgets
- [ ] Hero area (I/O TOKENS) at top 2 rows is unchanged
- [ ] Status label in footer chrome is unchanged
- [ ] 0 providers case: no tiles render, layout doesn't crash
- [ ] Run `python3 scripts/build/screenshot.py` for at least two provider configurations (e.g. 4 visible + 2 hidden, or full 12 visible) and confirm tile positioning matches the spec

## Phase 4: Hit-test + Input Wiring

### Overview
Rewrite `summary_hit_test()` to use grid cell arithmetic with `(x, y)` parameters. Update `handle_summary_event()` call site. Stop incrementing `auto_scroll_px` in `ui_task()`.

### Changes Required:

#### 5. firmware/main/ui_internal.h:257
**File**: firmware/main/ui_internal.h
**Changes**: MODIFY — Update `summary_hit_test()` declaration to take `(int x, int y)` alongside the definition change below

```c
int summary_hit_test(int x, int y);
```

#### 6. firmware/main/ui.c:61-85
**File**: firmware/main/ui.c
**Changes**: MODIFY — Rewrite `summary_hit_test()` from ticker-based to grid-cell-based

```c
// Translate screen (x,y) coordinates into a provider stats.p[] index using
// the fixed 2-column grid geometry. Returns -1 on miss (tap outside tile area
// or on an empty cell).
int summary_hit_test(int x, int y)
{
    const ui_page_grid_t g = ui_grid_from_height(s_scr_w, s_scr_h);
    int content_y0 = g.content.y + UI_SUMMARY_TOP_ROWS * g.cell_h;
    const ui_rect_t footer_slot = {
        .x = g.content.x,
        .y = g.content.y + g.content.h,
        .w = g.content.w,
        .h = UI_CHROME_BOTTOM,
    };
    if (y < content_y0 || y >= footer_slot.y) return -1;

    // Reject taps outside the content area horizontally
    if (x < g.content.x || x >= g.content.x + g.content.w) return -1;

    int col = (x - g.content.x) / g.cell_w;
    int row = (y - content_y0) / g.cell_h;
    if (col < 0 || col >= UI_GRID_COLS) return -1;
    if (row < 0 || row >= UI_GRID_ROWS - UI_SUMMARY_TOP_ROWS) return -1;

    int visible_idx = row * UI_GRID_COLS + col;
    return summary_provider_at(visible_idx);
}
```

#### 7. firmware/main/ui.c:291 (renumbered)
**File**: firmware/main/ui.c
**Changes**: MODIFY — Update `handle_summary_event()` call to pass `ev->x`

```c
        int pi = summary_hit_test(ev->x, ev->y);
```

#### 8. firmware/main/ui.c:143 (renumbered)
**File**: firmware/main/ui.c
**Changes**: MODIFY — Remove the `auto_scroll_px` increment line

Remove this line:
```c
                st.auto_scroll_px += AUTO_SCROLL_DELTA;
```

The surrounding context was:
```c
            // Automatic ticker scroll on the summary page: increment pixel
            // offset every frame and trigger a re-render for smooth animation.
            if (st.mode == UI_STATS && st.nav_level == NAV_SUMMARY) {
                st.auto_scroll_px += AUTO_SCROLL_DELTA;
                st.dirty = true;
            }
```
The `st.dirty = true` should stay (the nav_level check still gates the age-tick below). After removal:
```c
            if (st.mode == UI_STATS && st.nav_level == NAV_SUMMARY) {
                st.dirty = true;
            }
```

### Success Criteria:

#### Automated Verification:
- [ ] Build passes: `cd firmware && idf.py build`

#### Manual Verification:
- [ ] Tapping a tile navigates to the correct provider's card page
- [ ] Tapping hero area (rows 0-1) does nothing
- [ ] Tapping footer area does nothing
- [ ] Tapping an empty cell (no provider) does nothing
- [ ] Tapping the same tile again returns to the same provider's card
- [ ] Card entry animations (count-up, chart fade-in) still trigger correctly
- [ ] Swipe left from card page returns to summary grid
- [ ] No auto-scrolling — tiles stay stationary

## Ordering Constraints
- Phase 1 (Declarations) must come first — later phases depend on updated declarations
- Phase 2 (Tile Renderer) must come before Phase 3 (Render Loop) — the render loop calls `render_grid_tile()`
- Phase 4 (Hit-test + Input Wiring) applies after Phase 3 so the full feature is complete. It changes both the `summary_hit_test` declaration (`ui_internal.h:257`) and definition (`ui.c:61-82`) atomically to avoid a broken intermediate state.
- All phases are sequential; no parallel execution

## Verification Notes
- **Build**: `cd firmware && idf.py build` after each phase — `-Werror` means any warning is a build failure
- **Unit tests**: `cd firmware/test/ui_format && ./runtests` after Phase 2
- **Snapshot-backed validation**: Layout regressions are the #1 risk (precedent: 4 same-day fixes after last grid change). After all phases, use `python3 scripts/build/screenshot.py` to capture the summary page and verify tile positioning across provider configurations
- **Hit-test sync**: Render geometry and hit-test geometry must use the same grid constants (`UI_GRID_COLS`, `UI_GRID_ROWS`, `UI_SUMMARY_TOP_ROWS`, `cell_w`, `cell_h`). Mismatch = wrong provider opens on tap
- **Hidden providers**: Verify `ollama` and `opencode` are still hidden (no empty tiles left for them)
- **Empty cells**: With fewer than 12 visible providers, empty cells should render nothing. The hide-all preamble ensures this
- **Card transitions**: Ensure `st.prev_nav_level` reset still triggers entry animations (covered by existing code: `ui_render_core.c:901` sets `st.prev_nav_level = NAV_SUMMARY` on every summary render)

## Performance Considerations
- Removing the auto-scroll ticker eliminates per-frame `lv_obj_set_pos()` calls for every row — the grid render loop only shows current providers statically
- Grid render loop iterates max 12 cells (vs ticker's ROWS=12 + viewport calculations + fractional positioning)
- No more bar-pulse/cursor-pulse cleanup per frame for off-screen rows (animations that were stopped every frame)
- No more `lv_anim` overhead from stopping animations on hidden rows
- Net positive: fewer operations per frame, no continuous re-render requirement

## Migration Notes
Not applicable — no data migration or schema changes. This is a pure layout refactor of the summary page render path.

## Pattern References
- `ui_render_card.c:375-380` — Card page uses `ui_grid_from_height()` → `ui_grid_span()` pattern for grid-relative positioning
- `ui_render_core.c:313-319` — `place_hero_styled()` positions widgets relative to `ui_rect_t` (same pattern `render_grid_tile()` follows)
- `ui_render_summary.c:10-24` — `render_summary_secondary_bar()` is position-independent and unchanged
- `ui_format.c:10-30` — Grid geometry helpers used by both render and hit-test

## Developer Context
**Q (`ui.c:61`): `summary_hit_test()` currently takes `int y` but the 2-column grid needs both X and Y. Should the signature change to `int summary_hit_test(int x, int y)`?**
A: Yes, add the `x` param. The `app_evt_t` struct already carries both `.x` and `.y`. Only `handle_summary_event()` at `ui.c:291` calls this function.

**Directional: Reuse row_* arrays vs new tile_widgets_t struct**
A: Reuse existing row_* arrays. Minimal diff, less risk of regressions.

**Directional: File location for render_grid_tile()**
A: Keep in ui_render_summary.c. No new file needed.

**Directional: Remove dead constants vs leave in place**
A: Leave `AUTO_SCROLL_DELTA` and `auto_scroll_px` in place. Stop incrementing only.

## Plan Review (Step 8)

_Independent post-finalization review by artifact-code-reviewer and artifact-coverage-reviewer subagents. Findings triaged at Step 9._

| source   | plan-loc          | codebase-loc                | severity   | dimension             | finding   | recommendation   | resolution         |
| -------- | ----------------- | --------------------------- | ---------- | --------------------- | --------- | ---------------- | ------------------ |
| code     | Phase 2 §3        | ui_render_core.c:1047       | blocker    | actionability         | Phase 2 description says "Replace" but code fence only ADDS `render_grid_tile` alongside old `render_summary_row` — both coexist. The old function's definition stays untouched until Phase 3 removes the last call site. The phrase "Replace" is misleading. | Rephrase Phase 2 description to "ADD render_grid_tile alongside existing render_summary_row" — no code change needed. | dismissed — wording fixed to "ADD alongside" |
| code     | Phase 1 §1        | ui_internal.h:end           | suggestion | codebase-fit          | After all phases, `render_summary_row` declaration in `ui_internal.h` becomes orphaned (no definition after Phase 2, no callers after Phase 3) — dead declaration misleads readers. | Remove `void render_summary_row(...)` declaration from `ui_internal.h` as part of Phase 3 or Phase 4. | dismissed — harmless; cleanup in a future commit |
| coverage | ## Verification Notes §5 | <n/a>             | blocker    | verification-coverage | Note "Verify `ollama` and `opencode` are still hidden" has no Success Criteria bullet in any phase checking hidden-provider exclusion. | Add a manual verification bullet under Phase 3 or Phase 4: "Verify that hidden providers (ollama, opencode) produce neither a visible tile nor an empty cell gap." | dismissed — implicitly covered by "Empty cells show no ghost widgets" bullet |
| coverage | ## Verification Notes §3 | <n/a>             | concern    | verification-coverage | Note "use `python3 scripts/build/screenshot.py` to capture the summary page and verify tile positioning" has no Success Criteria bullet naming screenshots or cross-config validation. | Add a manual verification bullet under Phase 3: "Run `python3 scripts/build/screenshot.py` for at least two provider configurations (4 visible + 2 hidden, 12 visible) and confirm tile positioning." | applied — bullet added to Phase 3 Manual Verification |

## Plan History
- Phase 1: Declarations — approved as generated
- Phase 2: Tile Renderer — approved as generated
- Phase 3: Grid Render Loop — approved as generated
- Phase 4: Hit-test + Input Wiring — approved as generated

## References
- `.rpiv/artifacts/research/2026-06-04_08-28-47_summary-1x1-grid-tile-refactor.md` — Research artifact
- `.rpiv/artifacts/research/2026-05-28_23-27-26_display-widget-grid-refactor.md` — Original 2×8 grid refactor research
- `.rpiv/artifacts/plans/2026-05-28_23-54-57_display-widget-grid-refactor.md` — Grid refactor implementation plan
