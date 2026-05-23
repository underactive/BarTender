---
date: 2026-05-22T21:09:22-0700
author: Eric Sison
commit: 16e4fd2
branch: master
repository: bartender
topic: "Idle active-provider screensaver"
tags: [research, codebase, firmware-ui, screensaver, touch, display]
status: complete
last_updated: 2026-05-22T21:09:22-0700
last_updated_by: Eric Sison
---

# Research: Idle active-provider screensaver

## Research Question
Implement screensaver state inside `firmware/main/ui.c`, reusing the existing summary/provider-card render paths and `ui_handle_input()` event boundary. Add firmware-local provider metric snapshots/last-change timestamps, page-cycle state, saved navigation state, and backlight fade coordination from the UI task; keep the Upstash payload and host publisher unchanged.

## Summary
The feature should be firmware-only and concentrated in `firmware/main/ui.c`. Existing UI navigation is already mutex-protected, provider-ID-aware, and rendered only by the UI task, so screensaver entry, page cycling, dim fallback, and wake/restore behavior should extend the existing `st` state and dirty-render path rather than introduce a parallel renderer or fetch cadence. Provider activity can be detected at `ui_set_stats()` by comparing stable numeric fields from ok:true providers keyed by provider `id`; page rotation should use provider IDs, mirror summary hidden-provider filtering, and only include provider cards that have meaningful data.

## Detailed Findings

### UI-owned navigation and restore state
- `firmware/main/ui.c:35-52` defines the central shared UI state object, including `mode`, `stats`, `fetched_ms`, `dirty`, and navigation fields `nav_level`, `nav_provider`, `nav_id`, `nav_card`, and `scroll`.
- `firmware/main/ui.c:1040-1054` re-resolves `NAV_PAGE` by `st.nav_id` on each render so provider refresh reordering does not silently switch pages; if the provider disappears, render falls back to summary.
- `firmware/main/ui.c:1287-1335` is the single normal input boundary. It mutates navigation state under `s_mtx` and sets `st.dirty`; it performs no LVGL calls.
- Screensaver entry should save the full navigation tuple (`nav_level`, `nav_provider`, `nav_id`, `nav_card`, `scroll`) and restore it on wake. Restored `nav_provider` can be stale because render repairs it by `nav_id`.

### Idle detection and wake input
- `firmware/main/app_event.h:14-18` defines all semantic user inputs: tap, swipe up/down/left, and long press.
- `firmware/main/touch.c:72-185` classifies one physical press into exactly one `app_evt_t`; tap/long-press events carry coordinates, swipes use zero coordinates.
- `firmware/main/fetch.c:81-106` and `firmware/main/fetch.c:115-143` forward queued touch events to `ui_handle_input()` both during initial link wait and between fetches.
- `firmware/main/ui.c:1293-1295` returns `UI_INPUT_PASS` in provisioning mode; `firmware/main/ui.c:1317-1318` returns pass for long-press on the summary so `fetch.c` can open the add-network portal.
- Last interaction time should update for every real app event (`TAP`, all swipes, `LONG_PRESS`) inside `ui_handle_input()` after taking `s_mtx`. When screensaver/dim fallback is active, the first event should restore brightness/navigation, mark dirty, and return `UI_INPUT_CONSUMED` even for long press, preserving wake-only semantics.

### Provider activity detection
- `firmware/main/stats_model.h:26-59` exposes the comparable provider metric surface: `ok`, `has_p/p`, `has_s/s`, `has_t/t`, cost/token/balance fields, cost history, and percentage history.
- `firmware/main/stats_model.c:36` zeroes parsed output before population, so absent fields are false/zero and must be compared together with their `has_*` flags.
- `firmware/main/stats_model.c:74-86` parses provider id, ok, percentage fields, and reset strings; reset strings (`pr`, `sr`, `tr`) are volatile text and should not count as activity.
- `firmware/main/stats_model.c:90-119` parses generic `cost` numerics and history; `firmware/main/stats_model.c:127-163` maps Pi-specific `pi` metrics into the shared cost-shaped fields; `firmware/main/stats_model.c:168-176` parses provider `ph` usage history.
- `firmware/main/fetch.c:39-42` calls `ui_set_stats(&st, now_ms())` after successful parse, and `firmware/main/ui.c:1270-1277` copies stats into UI state. This is the right boundary to compare current ok:true provider samples against previous snapshots and stamp last-change uptime.
- Activity snapshots should be keyed by provider `id`, not array index, matching the existing navigation identity pattern at `firmware/main/ui.c:47-49` and `firmware/main/ui.c:1047-1054`.

### Screensaver page list generation
- `firmware/main/ui.c:1311-1314` opens provider pages by saving both provider index and `nav_id`, and initially chooses `CARD_COST` only when `has_cost` is true.
- `firmware/main/ui.c:1324-1327` cycles from Limits back to Cost only when the selected provider has `has_cost`.
- `firmware/main/ui.c:801-812` shows a Cost “not available” placeholder when `!p->has_cost`; screensaver should avoid enqueuing those pages.
- `firmware/main/ui.c:896-1007` renders the Limits card. A meaningful Limits page exists when `has_p || has_s || has_t || pct_hist_n > 0 || (has_cost && extra_limit_c > 0)`.
- `firmware/main/ui.c:124-132`, `firmware/main/ui.c:160-180`, and `firmware/main/ui.c:1125-1132` hide `ollama`, `opencode`, and `opencodego` from the summary. Developer decision: screensaver provider-card rotation should mirror summary visibility and exclude those hidden providers.

### Page-cycle timer and render discipline
- `firmware/main/ui.c:1182-1216` is the UI task loop. It wakes every 5 ms, takes `s_mtx`, runs time-based dirty logic, renders when `st.dirty`, releases the mutex, and runs LVGL timers.
- `firmware/main/ui.c:1186-1198` implements the existing 10-second `next_age` timer for the “updated Ns ago” summary label. It only marks dirty and does not affect fetch cadence.
- A screensaver next-page deadline should live beside `next_age` in `ui_task()`: when active and due, update navigation/page-cycle state by provider ID, set `st.dirty`, and let `render()` display the page.
- Touch responsiveness is independent of fetch cadence because `firmware/main/fetch.c:127-145` continues to service input while waiting for the next network poll.

### Backlight fade and dim fallback
- `firmware/main/display.c:216-217` restores configured brightness at display init using `config_store_get_brightness()`.
- `firmware/main/display.c:269-274` exposes `display_set_brightness(uint8_t duty)`, which synchronously sets LEDC duty and logs every call.
- Repeated fade steps through `display_set_brightness()` would log each step; a design should either add a silent/internal brightness helper or otherwise avoid log spam during UI-task-driven fades.
- `firmware/main/ui.h:1-9` documents that LVGL calls are UI-task-owned; fade stepping should also be coordinated from `ui_task()` to avoid blocking input and to keep page swaps/fade state serialized with render state.
- No active-provider fallback should save the current navigation state but leave the current page rendered, dim the backlight, and consume the next touch to restore configured brightness without applying the gesture to the restored page.

## Code References
- `firmware/main/ui.c:35-52` — shared UI state, including stats, dirty flag, navigation fields.
- `firmware/main/ui.c:124-132` — hidden provider IDs used by summary filtering.
- `firmware/main/ui.c:160-180` — compact visible-provider count and visible-index to provider-index mapping.
- `firmware/main/ui.c:784-1007` — provider Cost and Limits card renderer branches.
- `firmware/main/ui.c:1015-1066` — render dispatcher and provider-ID re-resolution.
- `firmware/main/ui.c:1085-1104` — summary status age text derived from `st.fetched_ms`.
- `firmware/main/ui.c:1182-1216` — UI task timing/render loop.
- `firmware/main/ui.c:1270-1277` — stats copy boundary from fetch/parser into UI state.
- `firmware/main/ui.c:1287-1335` — navigation input state machine and pass/consume contract.
- `firmware/main/stats_model.h:26-59` — provider metric fields available for activity snapshots.
- `firmware/main/stats_model.c:33-181` — parser population of provider metrics, histories, ok state, and ids.
- `firmware/main/fetch.c:81-106` — initial WiFi wait input forwarding.
- `firmware/main/fetch.c:115-143` — normal fetch interval input forwarding.
- `firmware/main/app_event.h:14-24` — semantic input event type contract.
- `firmware/main/touch.c:72-185` — touch classification into exactly one app event.
- `firmware/main/display.c:216-217` — boot-time configured brightness restore.
- `firmware/main/display.c:269-274` — runtime brightness setter with per-call logging.
- `firmware/main/ui.h:1-9` — UI-task LVGL ownership rule.

## Integration Points

### Inbound References
- `firmware/main/fetch.c:40-42` — successful fetch/parser path updates UI status and calls `ui_set_stats()`.
- `firmware/main/fetch.c:93-95` — initial link wait forwards input to UI and opens portal only on `UI_INPUT_PASS` long press.
- `firmware/main/fetch.c:137-139` — normal fetch loop forwards input and preserves portal long-press behavior.
- `firmware/main/touch.c:111-176` — emits long press, swipes, and taps onto the app queue consumed by fetch/UI.
- `firmware/main/main.c:61-69` — creates the event queue and wires touch producer plus fetch consumer.

### Outbound Dependencies
- `firmware/main/ui.c:1270-1277` — consumes `stats_t` from `stats_model` via `ui_set_stats()`.
- `firmware/main/ui.c:784-1007` — consumes `stats_provider_t` fields to render Cost/Limits cards.
- `firmware/main/ui.c:1056-1059` — calls LED provider state based on current page.
- `firmware/main/display.c:269-274` — hardware backlight operation available to UI screensaver fade/dim code.
- `firmware/main/config_store.c:114` — configured brightness source to restore after fade/dim.

### Infrastructure Wiring
- `firmware/main/ui.c:1182-1216` — UI task is the timer/render/fade coordination point.
- `firmware/main/ui.h:1-9` — enforces all LVGL work on the UI task and setter/input-only cross-task APIs.
- `firmware/main/app_event.h:3-10` — documents that fetch forwards every event to UI first and only acts on UI pass-through.

## Architecture Insights
- Screensaver is best modeled as a UI navigation overlay inside `firmware/main/ui.c` rather than a separate module, because render state, input handling, provider identity, dirty flags, and UI-task timing already live there.
- Provider activity is a firmware-local heuristic and should be session-only: keep previous metric snapshots and last-change timestamps in UI state, keyed by provider ID, and purge or ignore stale entries outside the 8-hour uptime window.
- Card availability must follow existing renderer predicates. Cost is exactly `has_cost`; Limits should require at least one numeric/graph/extra-usage branch to avoid empty placeholder cards.
- Screensaver page entries should store provider IDs and desired card kinds; immediately before rendering, re-resolve ID to current provider index using the same approach as normal `NAV_PAGE` rendering.
- Wake-touch handling must happen before normal navigation in `ui_handle_input()` and must return consumed so taps/swipes/long-presses do not affect the restored page or trigger setup portal.
- Backlight fade should be nonblocking state stepped from `ui_task()`; page swaps can render at low brightness then fade in, without blocking the fetch/input queue.

## Precedents & Lessons
5 similar past changes analyzed.

### Precedent: swipe navigation + provider card UI
**Commit(s)**: `31e4bf9` — "feat: swipe-nav menu + Claude Cost/Usage cards (payload v2) + provider theming" (2026-05-18)
**Blast radius**: 5 files across 4 layers
  firmware/ui/ — large navigation/card renderer expansion in `ui.c`
  firmware/model/ — provider stats fields/parser updates
  firmware/ipc-events/ — `app_event.h` input/event shape changed
  firmware/fetch/ — payload fetch handling changed

**Follow-up fixes**:
- `93bfd4b` — "Fix sub-1% provider value rendering as \"f%\" on the device" (2026-05-18) — tiny rendered values broke formatting.
- `7e884d7` — "fix: Codex 30d cost/tokens from codex-v fallback + chart flatline + hide extra usage" (2026-05-19) — charts and extra-usage visibility needed guards.
- `9a903b9` — "fix: extend today-cost date guard to Codex; skip TODAY page for providers without cost data" (2026-05-20) — no-data provider pages needed skipping.
- `8aead2c` — "fix: remove Codex today-cost date guard; limits card label + reset fixes" (2026-05-20) — earlier guard overconstrained UI.

**Lessons from docs**:
- `.rpiv/artifacts/research/2026-05-22_13-48-57_pi-agent-stats.md` — provider identity should be resolved by provider ID after refresh, not array index.
- `.rpiv/artifacts/discover/2026-05-22_21-00-11_idle-active-provider-screensaver.md` — screensaver cycling SHALL use provider IDs, not array indexes.

**Takeaway**: Page cycling must be ID-based and must skip unavailable provider cards.

### Precedent: scrollable summary + tap-cycle navigation redesign
**Commit(s)**: `3681e55` — "feat: scrollable summary + tap-cycle Cost/Limit pages (nav redesign)" (2026-05-18)
**Blast radius**: 3 files across 3 layers
  firmware/ui/ — navigation state and summary/card rendering changed
  firmware/fetch/ — fetch handling adjusted
  firmware/ipc-events/ — input/event definitions adjusted

**Follow-up fixes**:
- `e4188dc` — "fix(firmware): sync touch mirror flags to display flip orientation" (2026-05-19) — input/display orientation mismatch.
- `7e884d7` — "fix: Codex 30d cost/tokens from codex-v fallback + chart flatline + hide extra usage" (2026-05-19) — chart and visibility edge cases.
- `9a903b9` — "fix: extend today-cost date guard to Codex; skip TODAY page for providers without cost data" (2026-05-20) — page availability logic needed refinement.

**Lessons from docs**:
- `.rpiv/artifacts/discover/2026-05-22_21-00-11_idle-active-provider-screensaver.md` — exiting screensaver must restore the page/navigation state that was visible when screensaver began and consume the wake touch.

**Takeaway**: Save/restore full navigation state and treat wake touch as exit-only.

### Precedent: animated stats screens and timed render effects
**Commit(s)**: `e063f28` — "feat: animate stats screens — bar ease-in, red-zone pulse, count-up heroes, chart fade-in" (2026-05-20)
**Blast radius**: 1 file across 1 layer
  firmware/ui/ — timer/render animation logic added in `ui.c`

**Follow-up fixes**:
- `0c3a9ea` — "fix(ui): reuse autoscaled cost bar chart" (2026-05-22) — chart rendering path had to be unified/reused.
- `4613728` — "fix: improve Pi Agent provider reliability and contract alignment" (2026-05-22) — provider contract alignment affected UI assumptions.

**Lessons from docs**:
- `.rpiv/artifacts/discover/2026-05-22_21-00-11_idle-active-provider-screensaver.md` — LVGL calls must remain on the UI task; cross-task code should mutate UI state under the existing mutex and mark dirty.

**Takeaway**: Implement timers/fades inside the UI task path and reuse existing render functions.

### Precedent: Pi Agent provider UI/card rendering
**Commit(s)**: `f58693d` — "feat: add Pi Agent stats provider" (2026-05-22); `16e4fd2` — "feat: show today's Pi Agent usage" (2026-05-22)
**Blast radius**: 3 files across 2 layers
  firmware/model/ — Pi/provider stats fields and parsing changed
  firmware/ui/ — Pi labels, Today usage, and provider card rendering changed

**Follow-up fixes**:
- `4613728` — "fix: improve Pi Agent provider reliability and contract alignment" (2026-05-22) — provider contract/model alignment needed correction.
- `0c3a9ea` — "fix(ui): reuse autoscaled cost bar chart" (2026-05-22) — rendering reuse needed after provider-specific work.

**Lessons from docs**:
- `.rpiv/artifacts/designs/2026-05-22_14-16-27_pi-agent-stats.md` — keep Pi inside shared provider navigation/card chrome; do not add a separate screen path.
- `.rpiv/artifacts/plans/2026-05-22_14-23-07_pi-agent-stats.md` — verify non-Pi providers still render and navigate exactly as before.

**Takeaway**: Screensaver should drive existing summary/provider card states, not introduce provider-specific screensaver pages.

### Precedent: OpenRouter Today/Limits provider cards
**Commit(s)**: `6595eb9` — "feat: OpenRouter stats on Today and Limits cards" (2026-05-20)
**Blast radius**: 3 files across 2 layers
  firmware/model/ — provider fields parsed/extended
  firmware/ui/ — Today/Limits card rendering changed

**Follow-up fixes**:
- `78c9cfe` — "fix: OpenRouter Today layout + LEMONMILK hyphen glyph" (2026-05-20) — layout/font edge cases after card change.

**Lessons from docs**:
- `.rpiv/artifacts/research/2026-05-22_13-48-57_pi-agent-stats.md` — provider card additions tend to surface layout/font edge cases even when data shape is accepted.

**Takeaway**: Validate screensaver rotation on every included card type and provider label.

### Composite Lessons
- Use provider IDs, not provider array indexes, for screensaver cycle entries.
- Reuse existing UI navigation/render paths and save/restore full nav state.
- Skip unavailable Today/Cost pages for providers without cost data.
- Keep LVGL and fade coordination on the UI task path.
- Consume the first touch on screensaver exit to avoid accidental navigation or portal entry.

## Historical Context (from `.rpiv/artifacts/`)
- `.rpiv/artifacts/discover/2026-05-22_21-00-11_idle-active-provider-screensaver.md` — FRD and developer decisions for the screensaver feature.
- `.rpiv/artifacts/research/2026-05-22_13-48-57_pi-agent-stats.md` — prior research on provider identity/card rendering patterns.
- `.rpiv/artifacts/designs/2026-05-22_14-16-27_pi-agent-stats.md` — prior provider-card design artifact.
- `.rpiv/artifacts/plans/2026-05-22_14-23-07_pi-agent-stats.md` — prior provider-card implementation plan.

## Developer Context
**Q (discover: Foundational intent): For this screensaver feature, what problem are you solving and who is meant to benefit from it?**
A: Ambient glance

**Q (discover: UI-owned architecture): From the probe I inferred that screensaver navigation should live inside `firmware/main/ui.c` and reuse existing summary/card rendering state, while touch events continue through `ui_handle_input()` (`firmware/main/ui.c:44`, `firmware/main/ui.c:1015`, `firmware/main/ui.c:1287`). Keep that architecture, or change it?**
A: Keep in UI

**Q (discover: First-version scope): What should be explicitly in scope for this first screensaver version?**
A: Core screensaver — include idle entry, page cycle, active-provider heuristic, fade transition, touch exit, and restore-start-page; no settings UI or payload changes.

**Q (discover: Activity signal): For “active providers in the last 8 hours,” which interpretation should the FRD require?**
A: Firmware heuristic — provider is active when stable numeric usage/cost/token fields change while ok:true.

**Q (discover: Screensaver exit touch): When screensaver exits on touch, should that touch only exit, or also perform its normal action after restoring the starting page?**
A: Exit only — first touch wakes/restores and is consumed.

**Q (discover: Included provider pages): Which pages should the screensaver include for each recently active provider?**
A: Available cards — always include summary, then include Today/Cost only when provider has cost data, and Limits when provider has limit/usage data.

**Q (discover: Activity memory): How should the 8-hour activity window behave after a device reboot or a long offline gap?**
A: Session memory — track recent activity only since boot/while observing payload changes.

**Q (discover: No-active-provider idle fallback): What should the screensaver do if there are no providers considered active in the last 8 hours?**
A: Dim screen.

**Q (`firmware/main/ui.c:124-132`): The summary hides `ollama`, `opencode`, and `opencodego`. Should screensaver also exclude these hidden providers from provider-card rotation?**
A: Mirror summary — do not rotate hidden providers.

## Related Research
- `.rpiv/artifacts/research/2026-05-22_13-48-57_pi-agent-stats.md`

## Open Questions
None.
