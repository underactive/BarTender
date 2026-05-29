---
date: 2026-05-28T23:54:57-0700
author: Eric Sison
commit: 9dd894b
branch: main
repository: bartender
topic: "Display widget-grid refactor"
tags: [plan, firmware, ui, screenshot]
status: ready
parent: ".rpiv/artifacts/designs/2026-05-28_23-42-45_display-widget-grid-refactor.md"
last_updated: 2026-05-28T23:54:57-0700
last_updated_by: Eric Sison
---

# Display widget-grid refactor Implementation Plan

## Overview

This plan implements the firmware-only display renderer refactor described in `.rpiv/artifacts/designs/2026-05-28_23-42-45_display-widget-grid-refactor.md`. It replaces bespoke page-by-page positioning in `firmware/main/ui.c` with one shared 2x8 widget-grid model, keeps retained widgets and navigation behavior intact, and validates the layout with screenshots.

## Desired End State

The display renderer uses one shared 2x8 content grid for both summary and provider pages. Header/footer chrome stays outside the content grid, summary hit-testing and rendering share the same geometry helper, provider pages render as explicit grid spans, and screenshot captures confirm the layout without overlap.

## What We're NOT Doing

- Payload/schema changes.
- Navigation or gesture redesign.
- Host-side publisher changes.
- New modules outside `firmware/main/ui.c`.
- Alternative renderer rewrites that replace retained widgets with a new component system.

## Phase 1: Grid foundation + summary migration

### Overview

Establish the shared grid helper and migrate the summary renderer and hit-testing onto the new geometry model while preserving existing summary behavior.

### Changes Required:

#### 1. Shared grid helpers and summary geometry
**File**: `firmware/main/ui.c`
**Changes**: Add the shared 2x8 grid helpers and route summary row placement and hit-testing through the same geometry source, using cached screen dimensions rather than calling LVGL from the input path.

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
    const ui_page_grid_t g = ui_grid_from_height(s_scr_w, s_scr_h);
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

    const ui_page_grid_t g = ui_grid_from_height(s_scr_w, s_scr_h);
    render_summary_rows(&g);

    ...
}
```

### Success Criteria:

#### Automated Verification:
- [ ] Summary hit-testing and summary rendering share the same `ui_page_grid_t` helper: `grep -n "ui_grid_from_height\|ui_grid_span\|summary_hit_test\|render_summary_rows" firmware/main/ui.c` shows all four symbols in the same file.
- [ ] Summary page still compacts hidden providers and clamps scroll through the existing summary helpers.
- [ ] `ui_handle_input()` still opens pages from summary taps and still uses `summary_hit_test()` to choose providers.

#### Manual Verification:
- [ ] Summary page still shows the correct visible providers after a stats refresh.
- [ ] Tap targets follow the new grid and still navigate to the expected provider page.
- [ ] Header/status chrome remains outside the 2x8 content region.

---

## Phase 2: Provider migration + screenshot validation

### Overview

Re-express provider page rendering as explicit spans within the same shared grid and keep screenshot capture as the layout validation path.

### Changes Required:

#### 1. Provider page spans and screenshot-backed validation
**File**: `firmware/main/ui.c`
**Changes**: Update the provider-page rendering branches inside `render_card()` to place hero/chart/footer regions through the shared grid helper while preserving retained-widget mutation, provider re-resolution by `st.nav_id`, and screenshot capture behavior.

```c
static void render_card(void)   // ui_task only (renders the NAV_PAGE card)
{
    bool card_entered = (s_prev_nav_level    != NAV_PAGE        ||
                         s_prev_nav_provider != st.nav_provider ||
                         s_prev_nav_card     != st.nav_card);
    s_prev_nav_level    = NAV_PAGE;
    s_prev_nav_provider = st.nav_provider;
    s_prev_nav_card     = st.nav_card;

    hide_summary_chrome();

    const stats_provider_t *p = &st.stats.p[st.nav_provider];
    bool has_balance = (p->credits_limit_c > 0 || p->credits_remaining_c > 0);
    bool is_pi = (strcmp(p->id, "pi") == 0);
    bool is_lmstudio = (strcmp(p->id, "lmstudio") == 0);
    bool is_cursor = (strcmp(p->id, "cursor") == 0);

    if (st.nav_card == CARD_COST) {
        lv_obj_add_flag(lim_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cost_card, LV_OBJ_FLAG_HIDDEN);
        render_card_hdr(cost_hdr, cost_logo, p->id, "TODAY");

        ...

        const ui_page_grid_t g = ui_grid_from_height(s_scr_w, s_scr_h);
        const ui_rect_t hero = ui_grid_span(&g, 0, 0, 2, 2);
        const ui_rect_t body = ui_grid_span(&g, 0, 2, 2, 4);
        const ui_rect_t footer = ui_grid_span(&g, 0, 6, 2, 2);

        ...
    }
}
```

### Success Criteria:

#### Automated Verification:
- [ ] Provider page render branches continue to re-resolve the current provider by `st.nav_id` before drawing the page.
- [ ] Card-specific widget visibility resets still hide all non-applicable objects before each provider branch populates the shared grid.
- [ ] Screenshot capture still forces a full redraw before SCAP emission, so screenshot evidence reflects the new provider spans.

#### Manual Verification:
- [ ] Representative provider pages show their hero/chart/footer regions inside the shared 2x8 grid without overlap.
- [ ] Summary-to-provider navigation still returns the expected page after a stats refresh or provider reorder.
- [ ] Screenshot captures confirm header/footer chrome stays outside the content grid on both summary and provider pages.

---

## Testing Strategy

### Automated:
- Run the project’s existing firmware build and screenshot capture checks after the UI changes.
- Re-run the summary hit-test and provider re-resolution checks encoded in the phase success criteria.

### Manual Testing Steps:
1. Capture screenshots for the summary page and representative provider pages.
2. Verify that the header/footer chrome stays outside the content grid.
3. Verify that summary taps still open the expected provider page after a refresh or reorder.

## Performance Considerations

The refactor should keep the retained-widget approach, so render cost should stay close to current behavior. The main risk is extra layout bookkeeping; keep the grid math pure and cheap so summary hit-testing and page rendering remain lightweight.

## Migration Notes

Not applicable. This is a layout-only refactor with no persisted schema or payload migration.

## Developer Context

- Step 4 code review concern resolved: summary geometry uses cached screen dimensions instead of an LVGL call on the fetch/input path.
- Step 4 code review blocker resolved: Phase 2 now names concrete provider-span work in `render_card()`.

## References

- Design: `.rpiv/artifacts/designs/2026-05-28_23-42-45_display-widget-grid-refactor.md`
- Research: `.rpiv/artifacts/research/2026-05-28_23-27-26_display-widget-grid-refactor.md`
- Original ticket: `thoughts/me/tickets/display-widget-grid-refactor.md`

## Plan Review (Step 4)

_Independent post-finalization review by artifact-code-reviewer and artifact-coverage-reviewer subagents. Findings triaged at Step 5._

| source | plan-loc | codebase-loc | severity | dimension | finding | recommendation | resolution |
| ------ | -------- | ------------ | -------- | --------- | ------- | -------------- | ---------- |
| code | Phase 1 §1 (firmware/main/ui.c) | firmware/main/ui.c:538-545 | concern | code-quality | The proposed `summary_hit_test()` replaces a pure off-task helper with `lv_display_get_horizontal_resolution(lv_display_get_default())`, which introduces an LVGL call on the fetch/input task path and breaks the existing no-LVGL-off-task contract. | Keep the helper free of LVGL calls by passing the screen width in from `build_widgets()` or caching it alongside `s_scr_h`. | applied: use cached screen dimensions for summary geometry |
| code | Phase 2 §1 (firmware/main/ui.c) | <n/a> | blocker | actionability | Phase 2 does not actually specify any provider-span changes: the only fenced code block is the unchanged `render()` excerpt from Phase 1, so there is no concrete implementation target for the provider migration. | Add a Phase 2 file subsection with the explicit `render_card()` edits needed for the provider grid spans. | applied: add concrete `render_card()` provider-span targets |
