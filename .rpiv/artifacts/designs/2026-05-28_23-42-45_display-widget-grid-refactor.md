---
date: 2026-05-28T23:42:45-0700
author: Eric Sison
commit: 9dd894b
branch: main
repository: bartender
topic: "Display widget-grid refactor"
tags: [design, firmware, ui, screenshot]
status: ready
parent: .rpiv/artifacts/research/2026-05-28_23-27-26_display-widget-grid-refactor.md
last_updated: 2026-05-28T23:42:45-0700
last_updated_by: Eric Sison
---

# Design: Display widget-grid refactor

## Summary
Refactor the firmware display renderer in `firmware/main/ui.c` from bespoke page-by-page positioning into a single shared 2x8 widget-grid model with reserved header/footer chrome. The implementation stays firmware-only, keeps retained widgets and navigation semantics intact, and uses screenshot-backed validation to prove the summary and provider pages map cleanly onto reusable spans.

## Requirements
- Replace per-page manual positioning with reusable widget composition in the firmware UI seam.
- Use one shared 2x8 grid helper as the source of truth for summary and provider pages.
- Reserve header/footer chrome outside the 2x8 content grid.
- Keep navigation, gestures, payload shape, and provider stats semantics stable.
- Audit all currently rendered summary/provider pages and identify reusable widget spans.
- Use screenshot evidence from the existing capture path to validate the layout model.
- Stay scoped to the firmware display-rendering layer unless a later follow-up expands scope.

## Current State Analysis
The current renderer already retains LVGL objects and reuses them across renders, but the layout logic is spread across hard-coded coordinates and page-specific branches. Summary rows, hit-testing, and provider pages all read from the same `st` state, so the refactor can change layout math without changing the nav contract.

### Key Discoveries
- `firmware/main/ui.c:495-545` — summary visibility, scroll clamping, and hit-testing are already coupled to shared geometry; this is the best seam for a grid helper.
- `firmware/main/ui.c:593-942` — widgets are already retained and created once; the design should preserve this object lifecycle rather than introduce dynamic widget creation.
- `firmware/main/ui.c:944-977` — header/logo chrome is already factored into helper functions and can become the reserved page chrome anchor.
- `firmware/main/ui.c:1177-1518` — provider pages are currently branchy and position-driven; these branches need to be re-expressed as grid spans while keeping their data-driven differences.
- `firmware/main/ui.c:1556-1684` — provider identity is re-resolved by `st.nav_id`; the grid refactor must not break this reorder-safe behavior.
- `firmware/main/ui.c:1705-1880` — the UI task owns LVGL, screenshot sync, and state mutation boundaries; no new off-task rendering path should be introduced.
- `firmware/main/screenshot.c:27-65` and `scripts/screenshot.py:37-129` — the screenshot pipeline is already robust enough to audit layout changes, including binary framing and host-side decoding.
- `firmware/main/ui.h:14-28` — the navigation contract is intentionally stable and should not change as part of this refactor.

## Scope
### Building
- A shared 2x8 grid model for the main content area in `firmware/main/ui.c`.
- Re-anchoring summary rows to the new grid while preserving scroll, compaction, and tap-to-open behavior.
- Re-anchoring provider pages to explicit grid spans while preserving retained-widget mutation and identity-safe page selection.
- Screenshot-backed validation of the summary and provider layouts.

### Not Building
- Payload/schema changes.
- Navigation or gesture redesign.
- Host-side publisher changes.
- New modules outside `firmware/main/ui.c`.
- Alternative renderer rewrites that replace retained widgets with a new component system.

## Decisions
### 1) Grid ownership
**Decision**: Use one shared 2x8 content grid helper for all pages.
**Evidence**: The summary and provider branches already share a single renderer task and state container (`firmware/main/ui.c:1556-1684`, `firmware/main/ui.c:1705-1880`).
**Why**: A single grid source of truth keeps render math and hit-testing aligned and avoids page-specific layout drift.

### 2) Widget lifecycle
**Decision**: Keep retained LVGL widgets and mutate visibility/position in place.
**Evidence**: Widgets are created once in `build_widgets()` and reused across render passes (`firmware/main/ui.c:593-942`).
**Why**: This matches the existing single-owner UI pattern and minimizes risk to LVGL ownership and memory behavior.

### 3) Navigation contract
**Decision**: Preserve the current nav/data contract unchanged.
**Evidence**: `ui.h` already defines the 2-state nav machine and the summary/page behaviors (`firmware/main/ui.h:14-28`).
**Why**: The feature is a layout refactor, not a user-flow or payload change.

### 4) Screenshot validation
**Decision**: Require screenshot-backed validation for the new grid and page spans.
**Evidence**: The screenshot capture path is already synchronous and binary-safe (`firmware/main/screenshot.c:27-65`, `scripts/screenshot.py:37-129`).
**Why**: The layout audit needs visual evidence because the work is about spatial composition, not just code structure.

## Desired End State
Example summary-page model:

```c
// Shared chrome outside the grid; summary rows occupy grid bands.
page_chrome("BARTENDER", status_line);
widget_span(summary_row_0, 0, 0, 2, 1);
widget_span(summary_row_1, 0, 1, 2, 1);
```

Example provider-page model:

```c
page_chrome("CLAUDE  TODAY", provider_logo);
widget_span(hero_value, 0, 0, 2, 2);
widget_span(usage_chart, 0, 2, 2, 4);
widget_span(footer_caption, 0, 6, 2, 2);
```

## File Map
firmware/main/ui.c  # MODIFY — grid helpers, summary migration, provider-page migration, screenshot-friendly layout validation hooks

## Ordering Constraints
- Grid helpers must exist before summary and provider spans are rewritten.
- Summary hit-testing must use the same geometry helper as summary rendering.
- Provider page spans must be finalized after the shared chrome and grid boundaries are locked.
- Screenshot validation should run only after both summary and provider branches render through the new grid math.

## Verification Notes
- Confirm the summary page still scrolls and taps the same logical providers after re-anchoring.
- Confirm provider pages still re-resolve by `st.nav_id` and fall back to summary if the provider disappears.
- Capture screenshots for the summary page and representative provider pages to verify chrome reservation and lack of overlap.
- Check that LVGL calls remain on the UI task only.
- Keep the screenshot workflow in sync with the existing SCAP capture path; do not bypass `ui_capture_screenshot()`.

## Performance Considerations
The refactor should keep the retained-widget approach, so render cost should stay close to current behavior. The main risk is extra layout bookkeeping; keep the grid math pure and cheap so summary hit-testing and page rendering remain lightweight.

## Migration Notes
Not applicable. This is a layout-only refactor with no persisted schema or payload migration.

## Pattern References
- `firmware/main/ui.c:593-942` — retained widget construction pattern to preserve.
- `firmware/main/ui.c:944-977` — shared card header helper pattern to preserve.
- `firmware/main/ui.c:1556-1684` — identity-safe provider re-resolution and summary render split.
- `firmware/main/screenshot.c:27-65` — screenshot capture contract for layout validation.

## Developer Context
- Question: `firmware/main/ui.c:538-544` — `summary_hit_test()` mirrors the current row geometry in hit-testing. For the refactor, should tap targets stay pixel-identical to today, or can they change with the new grid as long as navigation still works?
  - Answer: Match new grid
- Question: Found 2 layout strategies for the new widget grid: one shared 2x8 template for every page, or separate summary/provider templates that share reserved chrome. Which should the design follow?
  - Answer: One shared grid
- Question: Design: display widget-grid refactor. Approach: keep the refactor firmware-only in `firmware/main/ui.c`, use one shared 2x8 grid helper as the source of truth for both summary and provider pages, preserve retained widgets and navigation semantics, and make screenshot-backed audit/validation part of the layout work. Decisions: shared grid for all pages; retained-widget mutation stays; nav/data contract unchanged; audit covers all current provider branches. Scope: building the grid foundation, summary migration, provider-page migration, and screenshot-backed validation; not building payload/schema changes or navigation changes. Files: 1 new, 1 modified. Ready to proceed to decomposition?
  - Answer: Proceed (Recommended)

## Design History
- Slice 1: Grid foundation + summary migration — pending
- Slice 2: Provider migration + screenshot validation — pending

## Architecture
### firmware/main/ui.c:1-1888 — MODIFY
Shared 2x8 grid helpers for summary/provider layout, with summary rendering and hit-testing re-anchored to the same content geometry.
```c
#define UI_GRID_COLS 2
#define UI_GRID_ROWS 8
#define UI_CHROME_TOP 20
#define UI_CHROME_BOTTOM 16
#define UI_SUMMARY_GAP 8

typedef struct { int x, y, w, h; } ui_rect_t;
typedef struct { ui_rect_t content; int cell_w; int cell_h; } ui_page_grid_t;

static ui_page_grid_t ui_grid_from_height(int screen_w, int screen_h)
{
    const int content_h = screen_h - UI_CHROME_TOP - UI_CHROME_BOTTOM;
    return (ui_page_grid_t){
        .content = { 0, UI_CHROME_TOP, screen_w, content_h },
        .cell_w = screen_w / UI_GRID_COLS,
        .cell_h = content_h / UI_GRID_ROWS,
    };
}

static ui_rect_t ui_grid_span(const ui_page_grid_t *g, int col, int row, int cols, int rows)
{
    return (ui_rect_t){
        .x = g->content.x + col * g->cell_w,
        .y = g->content.y + row * g->cell_h,
        .w = cols * g->cell_w,
        .h = rows * g->cell_h,
    };
}

static int summary_vis_rows_from_grid(const ui_page_grid_t *g)
{
    int rows = g->content.h / g->cell_h;
    if (rows < 1) rows = 1;
    if (rows > ROWS) rows = ROWS;
    return rows;
}

static int summary_hit_test(int y)
{
    const ui_page_grid_t g = ui_grid_from_height(lv_display_get_horizontal_resolution(lv_display_get_default()), s_scr_h);
    if (y < g.content.y || y >= g.content.y + g.content.h) return -1;
    const int slot = (y - g.content.y) / g.cell_h;
    if (slot < 0 || slot >= summary_vis_rows_from_grid(&g)) return -1;
    if ((y - g.content.y) % g.cell_h > g.cell_h - UI_SUMMARY_GAP) return -1;
    return summary_provider_at(st.scroll + slot);
}

static void render_summary_rows(const ui_page_grid_t *g)
{
    const int vis = summary_vis_rows_from_grid(g);
    for (int i = 0; i < ROWS; i++) {
        const int pi = summary_provider_at(st.scroll + i);
        if (i >= vis || pi < 0) {
            lv_obj_add_flag(row_id[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_bar[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_val[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_icon[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_bar_w[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const ui_rect_t r = ui_grid_span(g, 0, i, 2, 1);
        lv_obj_clear_flag(row_id[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(row_val[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(row_icon[i], r.x + 8, r.y + (r.h - ROW_ICON_PX) / 2);
        lv_obj_set_pos(row_id[i], r.x + ROW_TXT_X, r.y + 6);
        lv_obj_set_width(row_id[i], r.w - ROW_TXT_X - 8);
        lv_obj_set_pos(row_bar[i], r.x + ROW_TXT_X, r.y + 30);
        lv_obj_set_size(row_bar[i], r.w - ROW_TXT_X - 60, 7);
        lv_obj_set_pos(row_val[i], r.x + r.w - 52, r.y + 26);
        lv_obj_set_pos(row_bar_w[i], r.x + ROW_TXT_X, r.y + 39);
        lv_obj_set_size(row_bar_w[i], r.w - ROW_TXT_X - 60, 3);
    }
}

static void render(void)
{
    if (st.mode == UI_PROVISION) {
        ...
        return;
    }

    if (st.nav_level == NAV_PAGE) {
        render_card();
        return;
    }

    hide_cards();
    s_prev_nav_level = NAV_SUMMARY;
    lv_obj_add_flag(title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(prov_box, LV_OBJ_FLAG_HIDDEN);

    const int W = lv_display_get_horizontal_resolution(lv_display_get_default());
    const ui_page_grid_t g = ui_grid_from_height(W, s_scr_h);
    render_summary_rows(&g);

    ...
}
```

## Slices
### Slice 1: Grid foundation + summary migration

**Files**: `firmware/main/ui.c`

#### Automated Verification:
- [ ] Summary hit-testing and summary rendering share the same `ui_page_grid_t` helper: `grep -n "ui_grid_from_height\|ui_grid_span\|summary_hit_test\|render_summary_rows" firmware/main/ui.c` shows all four symbols in the same file.
- [ ] Summary page still compacts hidden providers and clamps scroll through the existing summary helpers.
- [ ] `ui_handle_input()` still opens pages from summary taps and still uses `summary_hit_test()` to choose providers.

#### Manual Verification:
- [ ] Summary page still shows the correct visible providers after a stats refresh.
- [ ] Tap targets follow the new grid and still navigate to the expected provider page.
- [ ] Header/status chrome remains outside the 2x8 content region.

### Slice 2: Provider migration + screenshot validation

**Files**: `firmware/main/ui.c`

#### Automated Verification:
- [ ] Provider page render branches continue to re-resolve the current provider by `st.nav_id` before drawing the page.
- [ ] Card-specific widget visibility resets still hide all non-applicable objects before each provider branch populates the shared grid.
- [ ] Screenshot capture still forces a full redraw before SCAP emission, so screenshot evidence reflects the new provider spans.

#### Manual Verification:
- [ ] Representative provider pages show their hero/chart/footer regions inside the shared 2x8 grid without overlap.
- [ ] Summary-to-provider navigation still returns the expected page after a stats refresh or provider reorder.
- [ ] Screenshot captures confirm header/footer chrome stays outside the content grid on both summary and provider pages.

## Design History
- Slice 1: Grid foundation + summary migration — approved as generated
- Slice 2: Provider migration + screenshot validation — approved as generated

## References
- `.rpiv/artifacts/research/2026-05-28_23-27-26_display-widget-grid-refactor.md`
- `.rpiv/artifacts/discover/2026-05-28_23-20-45_display-widget-grid-refactor.md`
- `firmware/main/ui.c`
- `firmware/main/ui.h`
- `firmware/main/screenshot.c`
- `scripts/screenshot.py`
