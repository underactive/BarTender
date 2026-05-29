---
date: 2026-05-28T23:20:45-0700
author: Eric Sison
commit: 9dd894b
branch: main
repository: bartender
topic: "Display widget-grid refactor"
tags: [intent, frd, firmware, ui, display]
status: complete
last_updated: 2026-05-28T23:20:45-0700
last_updated_by: Eric Sison
---

# FRD: Display widget-grid refactor

## Summary
Refactor the firmware display renderer away from page-by-page bespoke positioning and toward a reusable widget-based composition system. The new system should reserve shared header/footer chrome on every page, use a 2x8 grid for the main content region, and produce a screenshot-backed audit that identifies reusable widgets and grid spans across summary and provider pages.

The first pass is aimed at maintainer pain rather than changing device behavior. Navigation, gestures, payload shape, and provider stats semantics should stay stable while the layout/rendering seam in `firmware/main/ui.c` is reorganized into reusable page/widget definitions.

## Problem & Intent
"Maintainer pain"

The current device display pages are too bespoke in how they are laid out. I want to do a refactor on how pages on the device's display are rendered so each page's elements do not keep getting their own bespoke positioning. I want a widget-based system where each widget has its own grid size, the display is separated into a 2x8 grid, and every page has reserved header and footer space for page title/icon and max stats.

## Goals
- Replace bespoke per-page positioning with a reusable widget-based page composition system in the firmware UI layer.
- Define a 2-column by 8-row layout grid for the content region of device pages.
- Reserve shared header and footer bands on every page outside the 2x8 content grid.
- Audit the summary page and each provider page to identify reusable widgets and assign grid spans for each one.
- Make the audit screenshot-backed so layout decisions are grounded in captured on-device output.

## Non-Goals
- Changing navigation, gestures, page flow, or page-switching behavior.
- Changing payload shape, stats-model semantics, or host-side publishing behavior.
- Broad rendering-infrastructure work outside the current firmware display-rendering seam in `firmware/main/ui.c`.
- Treating the first pass primarily as a visual redesign instead of a maintainability refactor.

## Functional Requirements
1. The system SHALL refactor page composition within the firmware UI seam so that summary and provider pages are assembled from reusable widget definitions instead of ad hoc coordinate logic per page.
2. The system SHALL define a 2x8 grid for the main content region of each page.
3. The system SHALL reserve shared header and footer bands on every page, and those bands SHALL live outside the 2x8 content grid.
4. The system SHALL preserve the existing navigation model and page semantics while the layout refactor is introduced.
5. The system SHALL audit the summary page and every provider page currently rendered by the firmware UI.
6. The audit SHALL identify reusable widget types that can be shared across pages, including each widget's intended grid width and height.
7. The audit SHALL explicitly cover the examples already called out by the developer, including provider progress-bar widgets on the summary page and hero/graph/footer-style widgets on "Today" pages.
8. The feature SHALL produce a documented widget map/spec that records page-by-page widget usage and grid spans.
9. The audit and validation workflow SHALL use screenshot evidence captured through the existing screenshot toolchain.
10. The feature SHALL stay scoped to the firmware display-rendering layer unless a later follow-up explicitly expands scope.

## Non-Functional Requirements
- **Performance**: The refactor should preserve current device responsiveness and render behavior; no new requirement should depend on slower page-building paths or off-task LVGL usage.
- **Security**: No security-sensitive storage, credentials, or payload behavior should change; the feature remains firmware-layout-only.
- **UX / Accessibility**: The device should retain consistent page chrome and glanceable layouts, with shared header/footer treatment and predictable widget placement across pages.
- **Reliability**: The refactor should preserve existing page availability and semantics even for providers with partial data, since current page behavior is intentionally retained while only layout composition changes.

## Constraints & Assumptions
- The work is constrained to the firmware display-rendering seam centered in `firmware/main/ui.c`, where manual page geometry currently lives.
- All LVGL calls must remain on the single UI-owning task per project architecture.
- Existing navigation, gestures, and fetched/provider data contracts are assumed to remain stable during this refactor.
- The screenshot protocol and `scripts/screenshot.py` host tool are assumed to be available for audit evidence collection.
- The 2x8 grid applies to the central content region only; shared header/footer chrome sits outside that grid.
- The current screen types to audit are the summary view plus provider pages rendered today from the existing firmware implementation.

## Acceptance Criteria
- [ ] A new FRD-aligned implementation/design artifact names the reusable widget types for the summary page and each provider page, including the grid width and height of each widget.
- [ ] The documented layout model defines a 2-column by 8-row content grid and explicitly states that header/footer bands are reserved outside the grid.
- [ ] Review of the firmware UI seam shows that page composition is organized around reusable widget/page definitions rather than bespoke per-page coordinate blocks in the current style of `firmware/main/ui.c`.
- [ ] Existing navigation behavior still works: summary remains the root, tapping a provider still opens its page flow, tapping on a page still cycles cards, and swipe-left still returns to summary.
- [ ] No host-side payload/schema change is required; `stats_model` and publisher contracts remain unchanged for this feature.
- [ ] Screenshot evidence is captured with `python3 scripts/screenshot.py` for the audited pages and used to support the widget inventory and grid-span assignments.
- [ ] The summary page audit includes at least one reusable provider progress-bar widget spanning 2 columns by 1 row.
- [ ] The "Today" page audit includes reusable large-format widgets such as a hero widget spanning 2 columns by 2 rows, plus graph/footer-style widgets where applicable.

## Recommended Approach
Refactor the existing retained-widget firmware UI in `firmware/main/ui.c` into a reusable page-composition layer that defines shared page chrome, a central 2x8 content grid, and widget templates for summary/provider pages. Drive the first pass with a screenshot-backed audit and widget map so future implementation work can replace bespoke layout branches with reusable grid-based page definitions without changing data flow or navigation behavior.

## Decisions

### Primary problem framing
**Question**: What problem are you solving with this display-rendering refactor, and who feels the pain most today?
**Recommended**: n/a — `intent` question
**Chosen**: Maintainer pain
**Rationale**: The developer explicitly framed the work as reducing bespoke layout code and making the display renderer easier to evolve.

### Feature scope seam
**Question**: From the probe I inferred that this feature should stay narrowly scoped to the firmware display-rendering seam in `firmware/main/ui.c`, where summary rows and provider cards are currently positioned with manual `lv_obj_set_pos`/`lv_obj_set_size` calls (`firmware/main/ui.c:593-952`, `firmware/main/ui.c:1177-1522`). Keep that scope for the feature, or expand it now?
**Recommended**: Keep firmware-only
**Chosen**: Keep firmware-only
**Rationale**: evidence: `firmware/main/ui.c:593-952` and `firmware/main/ui.c:1177-1522` + confirmed

### Screenshot-backed audit
**Question**: From the probe I inferred that page-layout auditing should use the screenshot workflow as evidence, since the repo already has a dedicated capture path (`firmware/main/screenshot.c:27-29`) and the host tool at `scripts/screenshot.py:1-159`. Should the feature explicitly require screenshot-based auditing of each provider page and proposed reusable widgets?
**Recommended**: Yes, require it
**Chosen**: Yes, require it
**Rationale**: evidence: `firmware/main/screenshot.c:27-29` and `scripts/screenshot.py:1-159` + confirmed

### Primary first-pass goal
**Question**: For the first pass of this refactor, which outcome should be the primary goal?
**Recommended**: Shared widget inventory
**Chosen**: Shared widget inventory
**Rationale**: The developer wants the first pass to identify reusable widgets and grid sizes across pages so later implementation stops repeating bespoke positioning.

### Scope boundary
**Question**: Which scope boundary should we treat as a non-goal for this feature?
**Recommended**: Keep nav/data stable
**Chosen**: Keep nav/data stable
**Rationale**: The refactor is intended to change layout composition, not the device's gesture model, navigation model, or payload contract.

### Header/footer placement
**Question**: For the new layout system, where should the reserved header/footer bands live relative to the 2x8 page grid?
**Recommended**: Outside grid
**Chosen**: Outside grid
**Rationale**: This preserves consistent page chrome and keeps the 2x8 grid focused on reusable content widgets instead of reintroducing per-page competition for header/footer space.

### Audit deliverable
**Question**: What should count as acceptance for the provider-page audit portion of this feature?
**Recommended**: Spec + widget map
**Chosen**: Spec + widget map
**Rationale**: The developer wants the audit to produce an explicit reusable-widget inventory with grid spans, not leave those decisions implicit in code or screenshots alone.

## Open Questions
None.

## Suggested Follow-ups
- Summary-row tap hit-testing currently mirrors hard-coded row geometry in `firmware/main/ui.c:538-542`; if the refactor changes summary row geometry substantially, touch hit-testing may need a dedicated follow-up to derive from the new layout model.
- Provider-card render branches still include provider-specific repositioning in `firmware/main/ui.c:1219-1330`; a later implementation plan should decide whether those branches collapse entirely into declarative widget/page definitions or remain as data-selection logic only.

## References
- User feature description captured via `/skill:discover`
- `firmware/main/ui.c`
- `firmware/main/screenshot.c`
- `scripts/screenshot.py`
- `docs/product-specs/claude-cost-menu.md`
- `firmware/README.md`
