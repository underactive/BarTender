---
date: 2026-05-22T21:00:11-0700
author: Eric Sison
commit: 16e4fd2
branch: master
repository: bartender
topic: "Idle active-provider screensaver"
tags: [intent, frd, firmware-ui, touch, screensaver]
status: complete
last_updated: 2026-05-22T21:00:11-0700
last_updated_by: Eric Sison
---

# FRD: Idle active-provider screensaver

## Summary
Build an ambient-glance screensaver for the device that starts after 20 minutes of no user taps or swipes. While active, it cycles through the summary page and available pages for providers that the firmware has observed as recently active in the last 8 hours, using a backlight fade between pages and exiting on the next touch.

## Problem & Intent
Ambient glance.

The device should become a passive dashboard when nobody is interacting with it, surfacing recent provider activity without requiring manual navigation.

## Goals
- Turn idle time into a useful ambient dashboard instead of leaving the last manually selected page static.
- Highlight providers with recent observed activity over the last 8 hours using firmware-local comparison of published provider metrics.
- Preserve the user’s manual navigation context: exiting the screensaver restores the page that was visible when screensaver mode started.
- Keep the first implementation firmware-only: no host publisher, Upstash payload, schema, or settings changes.

## Non-Goals
- Do not add explicit host-published last-activity timestamps or active-window booleans.
- Do not add persisted or user-configurable screensaver timing in the first version.
- Do not deep-compare full provider objects including volatile reset strings as the activity signal.
- Do not add extra visual treatments beyond the requested backlight fade/page cycling behavior.
- Do not persist recent-activity state across reboot or long offline gaps.

## Functional Requirements
1. The firmware SHALL enter screensaver mode after 20 minutes with no user tap or swipe interaction while in normal stats mode.
2. The firmware SHALL treat a provider as recently active when selected stable numeric usage, cost, token, balance, or history fields change between successive successful payload observations while that provider is `ok:true`.
3. The firmware SHALL track provider recent activity using session memory only, with an 8-hour window measured from the device’s monotonic uptime at the moment a qualifying change is observed.
4. The screensaver SHALL cycle pages every 10 seconds.
5. The screensaver cycle SHALL always include the summary page when there is at least one recently active provider.
6. For each recently active provider, the cycle SHALL include the provider’s Today/Cost page only when that provider has cost-capable data.
7. For each recently active provider, the cycle SHALL include the provider’s Limits page when that provider has available usage/limit data.
8. The screensaver SHALL not show placeholder Today/Cost pages for providers that do not have cost-capable data, e.g. Cursor should show Limits only.
9. The firmware SHALL fade out and fade in between screensaver page swaps by changing backlight intensity.
10. The screensaver SHALL exit on the first touch event and consume that event rather than applying it as normal navigation.
11. When the screensaver exits, the firmware SHALL restore the page/navigation state that was displayed when screensaver mode started.
12. If no providers are considered active in the last 8 hours, idle behavior SHALL dim the current screen rather than cycling provider pages.
13. Screensaver page cycling SHALL use provider IDs, not provider array indexes, so refresh reordering does not silently switch providers.
14. Transient provider failures or `ok:false` payloads SHALL NOT count as provider activity.

## Non-Functional Requirements
- **Performance**: Screensaver bookkeeping should be lightweight enough to run within the existing UI/fetch cadence; no extra network fetch cadence is required.
- **Security**: No new data is published to Upstash and no raw local provider data is exposed; the feature consumes only the existing reduced payload already parsed by firmware.
- **UX / Accessibility**: The first touch during screensaver is a wake/exit gesture only. Page transitions should be visually smooth but not block responsiveness to touch.
- **Reliability**: Provider activity detection is a heuristic and must avoid obvious false positives from fetch failures or volatile reset-label churn. Reboot/offline gaps reset activity memory rather than attempting stale reconstruction.

## Constraints & Assumptions
- Existing navigation state lives in `firmware/main/ui.c` with `nav_level`, `nav_provider`, `nav_id`, `nav_card`, and `scroll` fields (`firmware/main/ui.c:44`).
- Existing rendering already supports summary and provider cards through the UI task (`firmware/main/ui.c:1015`).
- Existing touch events enter UI navigation through `ui_handle_input()` (`firmware/main/ui.c:1287`).
- LVGL calls must remain on the UI task; cross-task code should mutate UI state under the existing mutex and mark dirty.
- Existing display backlight control is available through `display_set_brightness()` (`firmware/main/display.c:269`), but repeated fade calls may require avoiding excessive logging.
- Existing stats provider fields include usage percentages, cost/tokens, histories, and `ok` status suitable for a numeric-change heuristic (`firmware/main/stats_model.h:18`).
- The device may not have reliable wall-clock time; 8-hour recent-activity tracking should use monotonic uptime observed by firmware.

## Acceptance Criteria
- [ ] Building the firmware with `cd firmware && idf.py build` exits 0.
- [ ] After 20 minutes with no tap or swipe in stats mode, the device enters screensaver behavior without requiring a fetch or reboot.
- [ ] With observed recent changes for Pi, Codex, and Cursor, the screensaver sequence visibly includes: summary, Pi Today, Pi Limits, Codex Today, Codex Limits, and Cursor Limits, with no Cursor Today placeholder.
- [ ] Each screensaver page remains visible for approximately 10 seconds before the next page transition begins.
- [ ] Each page swap visibly fades the backlight down and back up.
- [ ] Touching the screen while screensaver mode is active exits screensaver mode and restores the page that was visible when screensaver mode began.
- [ ] The touch that exits screensaver mode does not also tap, swipe, or long-press the restored page.
- [ ] A provider changing from `ok:true` to `ok:false`, or being absent due to a failed fetch, does not mark that provider active.
- [ ] After reboot, prior active-provider memory is not assumed; if no new provider changes have been observed for 8 hours, idle behavior dims the current page rather than cycling provider pages.

## Recommended Approach
Implement screensaver state inside `firmware/main/ui.c`, reusing the existing summary/provider-card render paths and `ui_handle_input()` event boundary. Add firmware-local provider metric snapshots/last-change timestamps, page-cycle state, saved navigation state, and backlight fade coordination from the UI task; keep the Upstash payload and host publisher unchanged.

## Decisions

### Foundational intent
**Question**: For this screensaver feature, what problem are you solving and who is meant to benefit from it?
**Recommended**: n/a — `intent` question
**Chosen**: Ambient glance
**Rationale**: Developer wants the idle device to become a passive dashboard for recent provider activity.

### UI-owned architecture
**Question**: From the probe I inferred that screensaver navigation should live inside `firmware/main/ui.c` and reuse existing summary/card rendering state, while touch events continue through `ui_handle_input()` (`firmware/main/ui.c:44`, `firmware/main/ui.c:1015`, `firmware/main/ui.c:1287`). Keep that architecture, or change it?
**Recommended**: Keep in UI — confirm the screensaver as UI navigation state; optimizes fit with existing LVGL ownership, loses a cleaner separate module boundary.
**Chosen**: Keep in UI
**Rationale**: evidence: `firmware/main/ui.c:44`, `firmware/main/ui.c:1015`, `firmware/main/ui.c:1287` + confirmed.

### First-version scope
**Question**: What should be explicitly in scope for this first screensaver version?
**Recommended**: Core screensaver — include idle entry, page cycle, active-provider heuristic, fade transition, touch exit, and restore-start-page; no settings UI or payload changes.
**Chosen**: Core screensaver
**Rationale**: Matches the requested behavior while avoiding payload/schema/config expansion.

### Activity signal
**Question**: For “active providers in the last 8 hours,” which interpretation should the FRD require?
**Recommended**: Firmware heuristic — provider is active when stable numeric usage/cost/token fields change while ok:true; optimizes no payload churn, loses exact last-used accuracy.
**Chosen**: Firmware heuristic
**Rationale**: Good enough for an ambient desk-toy dashboard and keeps the feature firmware-local.

### Screensaver exit touch
**Question**: When screensaver exits on touch, should that touch only exit, or also perform its normal action after restoring the starting page?
**Recommended**: Exit only — first touch wakes/restores and is consumed; optimizes predictable escape, loses one-tap immediate navigation.
**Chosen**: Exit only
**Rationale**: Prevents accidental navigation when the user only intended to stop the screensaver.

### Included provider pages
**Question**: Which pages should the screensaver include for each recently active provider?
**Recommended**: Available cards — always include summary, then include Today/Cost only when provider has cost data, and Limits when provider has limit/usage data; matches your Cursor example.
**Chosen**: Available cards
**Rationale**: Preserves useful pages while avoiding no-data placeholders for providers such as Cursor.

### Activity memory
**Question**: How should the 8-hour activity window behave after a device reboot or a long offline gap?
**Recommended**: Session memory — track recent activity only since boot/while observing payload changes; optimizes simplicity and no persistence, loses activity history across reboot/offline gaps.
**Chosen**: Session memory
**Rationale**: Avoids NVS persistence and accepts heuristic limitations for ambient display behavior.

### No-active-provider idle fallback
**Question**: What should the screensaver do if there are no providers considered active in the last 8 hours?
**Recommended**: Summary only — enter screensaver but cycle only the summary page with fade disabled or no-op; optimizes a stable ambient fallback, loses provider rotation.
**Chosen**: Dim screen
**Rationale**: Developer prefers a quiet/power-saving idle fallback when recent activity is unavailable.

## Open Questions

None.

## Suggested Follow-ups
- Consider adding persisted/configurable screensaver timings later if 20 minutes idle or 10 seconds per page need tuning.
- Consider a future publisher-derived activity timestamp only if the firmware heuristic proves misleading in practice.
- `display_set_brightness()` currently logs each brightness change (`firmware/main/display.c:269`); fade implementation may warrant a non-logging helper.

## References
- User-provided feature description in this discover invocation.
- `firmware/main/ui.c`
- `firmware/main/fetch.c`
- `firmware/main/display.c`
- `firmware/main/stats_model.h`
