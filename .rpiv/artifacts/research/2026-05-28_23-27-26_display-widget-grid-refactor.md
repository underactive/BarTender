---
date: 2026-05-28T23:27:26-0700
author: Eric Sison
commit: 9dd894b
branch: main
repository: bartender
topic: "Display widget-grid refactor"
tags: [research, firmware, ui, screenshot]
status: complete
last_updated: 2026-05-28T23:27:26-0700
last_updated_by: Eric Sison
---

# Research: Display widget-grid refactor

## Research Question
Refactor the firmware display renderer away from page-by-page bespoke positioning and toward a reusable widget-based composition system. The new system should reserve shared header/footer chrome on every page, use a 2x8 grid for the main content region, and produce a screenshot-backed audit that identifies reusable widgets and grid spans across summary and provider pages.

## Summary
The current UI is already split cleanly between a summary list and provider pages, but it is implemented as fixed-position LVGL object mutation rather than a declarative grid. Summary rendering and hit-testing share the same row geometry and compact-hidden-provider mapping, while provider pages reuse a small set of prebuilt widget objects and toggle them by branch. The screenshot path is a separate, robust audit channel: it forces a synchronous UI re-render, captures the shadow framebuffer, and decodes the SCAP stream on the host even through serial console CRLF expansion. The main new constraint from the checkpoint is that tap targets do not need to stay pixel-identical; they may follow the new grid as long as navigation still works.

## Detailed Findings

### Summary list pipeline
- `firmware/main/ui.c:495-534` computes visible summary rows from cached screen height, compacts out hidden providers, and clamps `st.scroll` to the compact-visible list rather than raw `st.stats.n`.
- `firmware/main/ui.c:538-545` maps a touch y-position back to a provider index using `ROW_Y0`, `ROW_H`, and the 8 px inter-row gap rule, then returns a raw `st.stats.p[]` index via `summary_provider_at()`.
- `firmware/main/ui.c:1575-1684` renders the summary page by walking preallocated row widgets and populating them from `st.scroll + i`; this is fixed-position object reuse, not a generic list container.
- `firmware/main/ui.c:1830-1858` uses the same compact-visible mapping for swipe paging, tap-to-open, and long-press passthrough in `ui_handle_input()`.
- Developer checkpoint answer: tap targets may change with the new grid, provided navigation semantics remain correct.

### Provider page lifecycle and shell
- `firmware/main/ui.h:14-28` defines the nav contract: `NAV_SUMMARY` vs `NAV_PAGE`, summary scroll/tap rules, page tap toggle, and swipe-left back navigation.
- `firmware/main/ui.c:31-67` stores page state in `st.nav_level`, `st.nav_provider`, `st.nav_id`, `st.nav_card`, and `st.scroll`.
- `firmware/main/ui.c:1556-1572` re-resolves the chosen provider by `st.nav_id` on each render so a reorder follows identity and a missing provider falls back to summary.
- `firmware/main/ui.c:1177-1518` dispatches into `render_card()` when `st.nav_level == NAV_PAGE`; the card branch is chosen by `st.nav_card` and then by provider shape.
- `firmware/main/ui.c:1186-1199` hides summary chrome before drawing the provider shell, and `render_card_hdr()` supplies the repeated header/logo pattern for both card families.

### Reusable widget inventory inside `ui.c`
- `firmware/main/ui.c:593-692` creates the summary chrome and row widgets once.
- `firmware/main/ui.c:679-800` builds the cost card and its shared pieces: `cost_card`, `cost_hdr`, `cost_logo`, `cost_big`, `cost_tok`, `cost_tok_unit`, `cost_30`, `cost_bar`, `cost_chart`, `cost_cap`, and the OpenRouter-specific row widgets.
- `firmware/main/ui.c:802-942` builds the limits card and its shared pieces: `lim_card`, `lim_hdr`, `lim_logo`, `lim_s_*`, `lim_a_*`, `lim_w_*`, `lim_x_*`, `lim_chart`, and `lim_cap`.
- `firmware/main/ui.c:944-977` implements `create_card_hdr()` / `render_card_hdr()`, which are the common header shell for both provider page families.
- `firmware/main/ui.c:1196-1518` does not create widgets dynamically; it selectively shows/hides and retitles the prebuilt objects.

### Layout exceptions and data-shape branches
- `firmware/main/ui.c:1153-1175` defines two helpers reused by multiple branches: `extra_pct()` for normalized extra usage and `set_reset_lbl()` for reset timestamp labels.
- `firmware/main/ui.c:1208-1237` gives LM Studio a cost-page “TODAY” mode with tokens, requests, a 30-day max footer, and a token history chart.
- `firmware/main/ui.c:1240-1261` gives Cursor a lighter “TODAY” variant with a chart and token history but fewer visible metrics.
- `firmware/main/ui.c:1268-1293` treats balance-bearing providers as a special layout where today’s money value moves into the hero, the remaining balance becomes the secondary metric, and the weekly summary is shown below.
- `firmware/main/ui.c:1294-1358` covers the generic Claude/Codex-style cost page and the Pi-specific positioning exception.
- `firmware/main/ui.c:1371-1405` gives LM Studio a separate limits/stats page with token and request usage.
- `firmware/main/ui.c:1409-1518` renders the generic limits page, branching on `has_balance`, `has_t`, `has_s`, `pct_hist_n`, and `extra_limit_c` to decide which shared widget regions are populated.
- `firmware/main/ui.c:1133-1149` is the shared chart renderer used by both LM Studio and generic cost-page charts.

### Screenshot-backed audit path
- `firmware/main/screenshot.h:1-10` declares the screenshot task API and documents the command-driven capture contract.
- `firmware/main/screenshot.c:27-65` listens for `screenshot\n`, calls `ui_capture_screenshot()`, pulls the shadow framebuffer, and emits an SCAP frame with magic, dimensions, length, and RGB565-LE pixels.
- `firmware/main/ui.c:1757-1764` makes capture synchronous by setting `st.shot_req` and waiting up to 2 seconds for the UI task to finish a full redraw.
- `firmware/main/display.c:34-38` and `firmware/main/display.c:286-290` define the shadow framebuffer as complete only after a full re-render.
- `scripts/screenshot.py:37-106` scans for SCAP magic, parses the frame header, and explicitly repairs CRLF-expanded serial payload bytes before collecting the logical pixel stream.
- `scripts/screenshot.py:109-129` converts the decoded RGB565-LE data into a PNG audit artifact.

### Integration Points

#### Inbound References
- `firmware/main/ui.h:14-28` — touch/input callers rely on the summary/page nav contract.
- `firmware/main/ui.c:1830-1880` — fetch-task input events drive summary scroll, tap-open, page toggle, and back navigation.
- `firmware/main/screenshot.c:27-65` — screenshot command path depends on `ui_capture_screenshot()`.
- `scripts/screenshot.py:1-160` — host-side audit tooling consumes the SCAP stream.

#### Outbound Dependencies
- `firmware/main/ui.c:495-545` — summary rendering and hit-testing depend on geometry constants and compact-hidden-provider mapping.
- `firmware/main/ui.c:1133-1518` — provider page branches depend on chart, bar, reset-label, and extra-usage helpers.
- `firmware/main/display.c:34-38` — screenshot capture depends on the shadow framebuffer being fully refreshed.

#### Infrastructure Wiring
- `firmware/main/ui.c:1705-1744` — the UI task owns all LVGL work, render invalidation, and screenshot completion signaling.
- `firmware/main/ui.c:1746-1807` — setters mark `st.dirty` and schedule redraws under the mutex.
- `firmware/main/ui.c:1816-1880` — the navigation state machine stays on the caller task and mutates only state, never LVGL.
- `firmware/main/screenshot.c:47-71` — the screenshot task is a separate FreeRTOS listener on stdin/USB serial.

## Code References
- `firmware/main/ui.c:495-545` — summary-visible row math, compact provider lookup, and touch hit-testing.
- `firmware/main/ui.c:593-942` — one-time construction of summary rows plus both provider card families.
- `firmware/main/ui.c:944-977` — shared card-header construction and rendering.
- `firmware/main/ui.c:1153-1518` — shared helpers plus all provider-specific cost/limits page branches.
- `firmware/main/ui.c:1556-1684` — provider-page dispatch and summary rendering.
- `firmware/main/ui.c:1705-1880` — UI task loop, screenshot sync, setters, and nav state machine.
- `firmware/main/screenshot.c:27-65` — firmware-side screenshot framing and binary send path.
- `scripts/screenshot.py:37-129` — host-side SCAP scan, CRLF repair, and PNG conversion.

## Architecture Insights
- The current renderer already has reusable object instances, but their behavior is encoded imperatively through visibility and label mutation rather than declarative page/widget definitions.
- Summary geometry, scroll paging, and tap hit-testing are tightly coupled; any grid refactor needs to update render and input geometry together.
- Provider pages are identity-stable through `st.nav_id`, so the page model is already resilient to reorder and should stay that way.
- The screenshot workflow is robust enough to support layout audits, but it depends on a synchronized UI redraw and on the host decoding the serial stream defensively.
- Because the checkpoint allowed tap targets to change with the grid, the refactor can treat the new grid as the source of truth rather than preserving the old pixel contract.

## Precedents & Lessons
4 similar past changes analyzed.

### Precedent: USB-serial screenshot capture infrastructure
**Commit(s)**: `b7ca19a` — "feat(firmware): USB-serial screenshot capture" (2026-05-??)
**Blast radius**: 4 files across 3 layers
- `firmware/main/screenshot.c` — new firmware-side screenshot responder
- `firmware/main/ui.c` — UI hook for capture path
- `firmware/main/ui.h` — capture/API contract
- `scripts/screenshot.py` — host-side capture client

**Follow-up fixes**:
- `28d54b7` — "Fix screenshot stream decoding" (2026-05-??) — host capture decoding was brittle and needed a protocol fix shortly after introduction

**Lessons from docs**:
- `.rpiv/artifacts/discover/2026-05-28_23-20-45_display-widget-grid-refactor.md` — the feature explicitly depends on screenshot evidence for widget mapping and grid-span validation
- `.rpiv/guidance/scripts/architecture.md` — screenshot.py is a serial protocol client; keep host/device protocol assumptions narrow and explicit

**Takeaway**: Validate the capture stream and framing early, because the evidence channel itself has already needed a protocol fix.

### Precedent: Large firmware UI layout overhaul with normalized rendering
**Commit(s)**: `3c8543e` — "feat(firmware): redesign Limits card UI + normalize bar rendering" (2026-05-??)
**Blast radius**: 1 file across 1 layer
- `firmware/main/ui.c` — major render/layout rewrite in the display seam

**Follow-up fixes**:
- `20f42d3` — "fix: overlap of bottom text lines with bar chart on TODAY page" (2026-05-??) — layout collision after the redesign
- `70efbf1` — "fix: chart-to-text overlap on LM Studio and Cursor TODAY pages" (2026-05-??) — another overlap fix in the same rendering area

**Lessons from docs**:
- `.rpiv/artifacts/discover/2026-05-28_23-20-45_display-widget-grid-refactor.md` — the current renderer still uses bespoke positioning in `ui.c`; the refactor is meant to replace that style, not add more per-page geometry
- `.rpiv/artifacts/discover/2026-05-28_23-20-45_display-widget-grid-refactor.md` — geometry changes can cascade into hit-testing follow-up work

**Takeaway**: Rendering rewrites in `ui.c` have a history of overlap regressions, so grid spans and vertical spacing need explicit validation.

### Precedent: New page-cycle/navigation state layered into UI
**Commit(s)**: `5da5c88` — "feat: idle active-provider screensaver with page cycling, dim fallback, and summary wrap-around" (2026-05-??)
**Blast radius**: 2 files across 2 layers
- `firmware/main/ui.c` — substantial navigation/state machine expansion
- `firmware/main/ui.h` — input contract documentation update

**Follow-up fixes**:
- `280e632` — "fix: cycle LM Studio Today/Stats unconditionally on tap" (2026-05-??) — navigation wrap/cycle behavior needed a correction after feature land

**Lessons from docs**:
- `.rpiv/artifacts/plans/2026-05-22_21-34-31_idle-active-provider-screensaver.md` — UI-owned state and input handling were intentionally kept inside `ui.c`; keep layout/navigation ownership localized to avoid cross-task complexity
- `.rpiv/artifacts/research/2026-05-22_21-09-22_idle-active-provider-screensaver.md` — wake/input contracts matter; `ui.h` was used to document behavior changes and avoid accidental pass-through

**Takeaway**: Keep the refactor localized to the UI seam and preserve the single-owner task model.

### Precedent: Summary/card navigation redesign around provider pages
**Commit(s)**: `3681e55` — "feat: scrollable summary + tap-cycle Cost/Limit pages (nav redesign)" (2026-05-??)
**Blast radius**: 1 file across 1 layer
- `firmware/main/ui.c` — navigation and page cycling logic

**Follow-up fixes**:
- `280e632` — "fix: cycle LM Studio Today/Stats unconditionally on tap" (2026-05-??) — confirms tap-cycle logic was easy to get subtly wrong

**Lessons from docs**:
- `.rpiv/artifacts/designs/2026-05-23_12-04-10_lm-studio-stats-on-device.md` — provider/page behavior is tightly coupled to UI state and hidden-provider rules
- `.rpiv/artifacts/discover/2026-05-28_23-20-45_display-widget-grid-refactor.md` — provider-card render branches still contain provider-specific repositioning; a later implementation should decide whether those branches become declarative widgets or remain data selection only

**Takeaway**: Treat navigation state as part of the layout contract, not as a separate concern.

### Composite Lessons
- Screenshot-backed work is only as good as the capture protocol; validate the SCAP stream before relying on layout screenshots.
- UI render rewrites in `firmware/main/ui.c` have repeatedly produced overlap or cycling regressions; validate spacing, page transitions, and page toggles with on-device screenshots.
- Preserve UI-task ownership and the identity-stable provider page model while replacing bespoke positioning with grid-based composition.

## Historical Context (from `.rpiv/artifacts/`)
- `.rpiv/artifacts/discover/2026-05-28_23-20-45_display-widget-grid-refactor.md` — feature framing, goals, non-goals, and explicit screenshot-backed audit requirements.
- `.rpiv/artifacts/designs/2026-05-23_12-04-10_lm-studio-stats-on-device.md` — prior provider/page design context around hidden-provider and stats-page behavior.
- `.rpiv/artifacts/plans/2026-05-22_21-34-31_idle-active-provider-screensaver.md` — prior phased work on page cycling and UI task ownership.
- `.rpiv/artifacts/research/2026-05-22_21-09-22_idle-active-provider-screensaver.md` — prior research on wake/input contracts and UI state ownership.

## Developer Context
**Q (`firmware/main/ui.c:538-544`): `summary_hit_test()` mirrors the current row geometry in hit-testing. For the refactor, should tap targets stay pixel-identical to today, or can they change with the new grid as long as navigation still works?**
A: Match new grid

## Related Research
- `.rpiv/artifacts/discover/2026-05-28_23-20-45_display-widget-grid-refactor.md`

## Open Questions
None.
