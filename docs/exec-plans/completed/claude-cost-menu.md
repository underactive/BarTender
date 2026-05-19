# Plan: Swipe-navigated provider menu with Claude Cost & Usage-Limits cards

- **Started:** 2026-05-18
- **Status:** Completed 2026-05-18 (code + `idf.py build` + 7-persona audit
  done; on-device gesture/render verification pending user flash)
- **Objective** — Add a touch swipe-down menu (Summary + providers) → submenu
  (Cost / Usage Limits) → detail cards, with real Claude cost/usage data, on the
  ESP32 CodexBar toy.
- **Changes** — see *Changes* table below.
- **Dependencies** — A.1 (schema capture, DONE) unblocked everything; firmware
  must accept payload v2 before the publisher emits it (R2).
- **Risks / open questions** — see *Risks* and *Open questions*.

Companion: approved plan at `~/.claude/plans/add-a-menu-that-proud-tide.md`.
This doc supersedes that plan's Workstream A data-source design (the A.1 capture
disproved its premise — see Decisions 2026-05-18 #2).

## Context

The toy shows a single summary screen of provider usage %. The user wants a
swipe-down menu → per-provider submenu → Cost / Usage-Limits cards, navigated by
touch (swipe down = descend, swipe up = back, tap = select). Build Claude fully;
stub Codex/Cursor/OpenRouter. The toy + Upstash are private to one user, so the
publish boundary is deliberately relaxed (raw data incl. PII may transit) — this
must be re-justified in `docs/SECURITY.md`, with the unencrypted-NVS residual
risk logged as hardening debt.

## Decoded data sources (the non-obvious core — captured 2026-05-18)

`codexbar usage --provider claude --source auto --format json` returns ONLY:
`usage.primary` (session, `windowMinutes` 300, `usedPercent`, `resetsAt`),
`usage.secondary`/`tertiary` (weekly windows), `usage.extraRateWindows[]`
(Designs / Daily Routines, `usedPercent`), `usage.providerCost`
(`{used,limit,period:"Monthly",currencyCode}` = the **Extra-usage overage** $,
NOT total spend), and identity/email/org/loginMethod (PII). Source `cli` returns
windows only (no `providerCost`). **No today/30d $, no tokens, no cost history.**

The headline Cost ("Today $ · tokens / Last 30 days $ · tokens" + bar chart)
lives in CodexBar's internal cache, built from local `~/.claude/**/*.jsonl`:

`~/Library/Caches/CodexBar/cost-usage/claude-v2.json`
```
{ lastScanUnixMs, version:1,
  files: { "<ABS PROJECT PATH — PII, 2100 entries>": { claudeRows:[…], … } },
  days:  { "YYYY-MM-DD": { "<model>": [in, cacheRead, cacheCreate, out, costNanos, rows, rows] } } }
```
Array order **verified** against raw `claudeRows` (fields
`costNanos,input,output,cacheRead,cacheCreate,model,dayKey`) for two cells:
`days[d][m] = [input, cacheRead, cacheCreate, output, costNanos, rows, rows]`.
- Day $ = `Σ_models arr[4] / 1e9` (costNanos→USD).
- Day tokens = `Σ_models (arr[0]+arr[1]+arr[2]+arr[3])`.
- 30-day history = per-day $ over the day keys (cache holds ~20–31 days).
- **`days` is clean aggregates (no PII). `files` keys are private project
  paths — the publisher must roll up and forward ONLY aggregates, never `files`.**
- Filename suffix churns across CodexBar releases (`-v1/-v2/-v4/-v6`); guard on
  filename + inner `version` and degrade gracefully if unrecognized (R1).

Clean per-window usage-% time series (for a real session/weekly sparkline):
`~/Library/Application Support/com.steipete.codexbar/history/claude.json` →
`accounts.<hash>[].entries[] = {capturedAt, resetsAt, usedPercent}` (~hourly).

Capture fixtures (local-only, `.gitignore`d): `docs/references/codexbar-usage-claude.sample.json`,
`docs/references/codexbar-cost-cache-claude.sample.json` (PII-redacted).

## Changes

| File | Change |
|---|---|
| `scripts/codexbar-stats.sh` | Drop JXA whitelist `project()`; emit `{v:2,ts,providers:[<raw codexbar usage obj>]}` for enabled providers; keep enabled-selection + exit semantics; drop the now-void no-`$`/PII assertion |
| `scripts/codexbar-publish.sh` | After the usage snapshot, parse `cost-usage/claude-v*.json` (filename+`version` guard, fallback=omit cost), roll up today/30d $+tokens + per-day $ history from `days` only; merge a `cost` block into the Claude provider; optionally fold `history/claude.json` into a usage-% series; keep last-known-good fail-safe; `0600` lockfile |
| `firmware/main/stats_model.h` | Add to `stats_provider_t`: `has_cost`; `cost_today_cents`,`cost_month_cents`; `tok_today`,`tok_month` (scaled ints); `cost_extra_used_cents`,`cost_extra_limit_cents`; `hist30[31]`/`hist30_n` (daily $ cents); optional `pct_hist[]`. New `STATS_*` consts |
| `firmware/main/stats_model.c` | Parse v2 paths via existing `cpy()`/`has_*` pattern (`:9-15,:61-66`); accept `v==1\|\|v==2` at guard (`:49`); `memset` (`:20`) zero-inits |
| `firmware/main/fetch.c` | Enlarge `body[]` (measure real bytes; `upstash.c` oversize still fails safe); make local `stats_t` (`:33`) `static` |
| `firmware/main/app_event.h` | `APP_EVT_TAP=1{int16 x,y}` + `APP_EVT_SWIPE_UP/DOWN` |
| `firmware/main/touch.c` | Press→move→release gesture FSM; keep `DEBOUNCE_US` (`:36`); thresholds tap ≤12px/≤700ms, swipe ≥40px/≤600ms/vertical, 12–40px dead-zone, 40ms dropout grace |
| `firmware/main/ui.c`/`ui.h` | Nav state in mutex `st`; `ui_handle_input()`→PASS/CONSUMED (PASS only on `NAV_SUMMARY`+tap); pre-create menu/submenu/cards (hidden); per-level `render()` branches; `menu_hit_test`; 30-day `lv_chart` BAR + (optional) usage-% LINE; reuse `pct_color()` (`:32-37`) + integer-tenths money/token fmt (`:162-173`); ASCII-only |
| `docs/SECURITY.md` | Rewrite §"Project exception": relaxed single-user model; raw incl. PII transits; cost cache parsed locally, only aggregates forwarded (never `files`); NVS-encryption hardening debt |
| `docs/generated/codexbar-payload.schema.json` (+README), `ARCHITECTURE.md` | v2 schema doc + contract snippet + decision entry |
| `docs/product-specs/` (`esp32-toy.md`, new spec, `index.md`), `docs/QUALITY_SCORE.md`, `docs/exec-plans/tech-debt-tracker.md`, `README.md`, `docs/testing-checklist.md` | AGENTS.md steps 8–12 |

## Steps

- [x] A.1 Capture authoritative schema; decode cost-cache array; save fixtures
- [x] Create exec-plan + PLANS.md index row
- [x] B firmware contract: stats_model.h/.c (v2, accept v1‖v2), fetch.c body/static
- [x] A.2 codexbar-stats.sh raw passthrough (v2)
- [x] A.3 codexbar-publish.sh cost-cache rollup + merge + guard + lockfile + fail-safe
- [x] A.4 schema doc + ARCHITECTURE contract snippet
- [x] C app_event.h + touch.c gesture FSM
- [x] D ui.c/ui.h nav FSM + widgets + cards + chart + hit-test; fetch.c gate
- [x] E SECURITY.md re-justify + product spec + ARCHITECTURE decision + QUALITY/tech-debt/README/testing-checklist
- [x] idf.py build clean (rc=0, 1.38 MB); pipeline verified vs live cache + fallbacks
- [x] docs/AUDIT.md 7-persona reviewers run; fixes applied; report appended
- [x] Plan moved active/ → completed/; PLANS.md updated
- [ ] (USER) on-device verification per docs/testing-checklist.md "v2 cost menu"

## Implementation summary

Built as designed, with these decisions/deviations (all in *Decisions*):
- `codexbar usage` does NOT carry cost → publisher rolls it up from CodexBar's
  local cost cache (Decision #2). Schema decoded + verified.
- Firmware owns a stable flat **v2** contract (strict superset of v1) instead
  of forwarding CodexBar's nested JSON to the MCU (Decision #8 in ARCHITECTURE).
- 24h *hourly* cost not sourceable → Cost card = TODAY big number + real
  30-day daily bar chart (Decision #3).
- `int32_t` is `long` on Xtensa → money formatter uses explicit `(int)` casts
  (caught by `idf.py` `-Werror=format`, fixed).

### Files changed

- `firmware/main/stats_model.h` — v2 cost fields + `STATS_HIST_MAX`, stdint
- `firmware/main/stats_model.c` — accept `v==1‖v==2`; parse `cost{ct,cm,tt,tm,xu,xl,h}`
- `firmware/main/fetch.c` — `body[4096]`, `static stats_t`, delegate to `ui_handle_input()` + PASS gate
- `firmware/main/app_event.h` — `APP_EVT_TAP{x,y}` + `SWIPE_UP/DOWN`
- `firmware/main/touch.c` — press→move→release gesture FSM (DEBOUNCE_US unchanged)
- `firmware/main/ui.h` / `ui.c` — nav state machine, menu/submenu/Cost+Limits widgets, `lv_chart`, `menu_hit_test`, `ui_handle_input()`
- `scripts/codexbar-stats.sh` — v2 projection (+`cost{xu,xl}`), `v:1`→`v:2`
- `scripts/codexbar-publish.sh` — `COST_MERGE_JXA` cost-cache rollup, single-flight lock, churn fail-safe
- `docs/SECURITY.md`, `ARCHITECTURE.md`, `docs/generated/codexbar-payload.schema.json` (+README), `docs/QUALITY_SCORE.md`, `docs/exec-plans/tech-debt-tracker.md`, `docs/product-specs/{claude-cost-menu.md,index.md,esp32-toy.md}`, `docs/testing-checklist.md`, `README.md`, `docs/PLANS.md`
- `docs/references/*.sample.json` + `.gitignore` (local-only fixtures, PII-redacted)

### Verification

- `idf.py build` → **rc=0**, `codexbar-toy.bin` 0x14fc20 (~1.38 MB), 83% free.
- `codexbar-stats.sh --json` → valid `v:2` (claude p/pr/s/sr + `cost{xu,xl}`).
- Cost-merge JXA vs live cache → today `$87.29`/123M tok, 30d `$623.63`/855M
  tok, 20-day history; compact payload 589 B.
- Fail-safe: empty cache dir & churned schema → exit 3, payload uncorrupted,
  publisher logs note + would publish usage-only.
- On-device gesture/render checklist: **pending user flash** (see
  `docs/testing-checklist.md` → "Firmware — v2 cost menu / swipe nav").

### Follow-ups

See `docs/exec-plans/tech-debt-tracker.md`: NVS encryption (risk elevated),
CodexBar cache-format coupling, no host harness, per-model breakdown deferred.

## Audit Report (2026-05-18)

Seven-persona audit (QA, Security, Interface, State, Resource/Concurrency,
Testing, DX) over the changed files + immediate dependents.

**Files with findings:** `firmware/main/stats_model.c`, `firmware/main/ui.c`,
`scripts/codexbar-publish.sh` (pre-existing, see below).

Verdicts: Interface — no findings. State — no findings (mutex discipline,
dirty-flag coverage, and the no-LVGL-off-ui_task property all confirmed).
Resource/Concurrency — no critical findings (FreeRTOS mutex has priority
inheritance; stacks adequate; cJSON/temp/lock cleanup correct on all paths).

Actioned:
- `[FIXED]` Security§MED — `stats_model.c` cast `double→int32/int64` for
  cost/token/hist without bounds → silent wrap on a corrupt store value. Added
  `i32_clamp`/`i64_clamp`.
- `[FIXED]` DX§MED — extra-usage % math duplicated in the Cost and
  Usage-Limits cards → extracted `extra_pct()` (single source).
- `[FIXED]` DX§LOW — added clarifying comments: `NAV_HIST_PTS` ↔ schema cap,
  `ui_handle_input` ↔ ARCHITECTURE decision #9, publisher history slice
  oldest→newest ↔ `stats_model.h hist[]`.

Reviewed and dismissed (false alarms — code re-read):
- QA/Security "div-by-zero in CARD_LIMITS / Cost bar": both divisions are
  inside an `extra_limit_c > 0` guard (and now `extra_pct()` centralizes it).
- QA "RELEASE_GRACE off-by-one": pre-increment then `>= 2` commits on the 2nd
  consecutive no-touch poll = the intended 40 ms grace. Correct.
- QA/Security "hist off-by-one / array overflow": `>= STATS_HIST_MAX` break
  precedes the write; stores exactly 31, array size 31. Correct.
- QA "render_card unguarded nav_provider": render() validates and resets to
  NAV_MENU in the same s_mtx critical section before dispatching. Safe.
- Security "uint16→int16 coord truncation": panel is ≤320 px; cannot exceed
  INT16_MAX. Accepted as-is.

Deferred / logged as debt (not fixed here):
- Security§HIGH "plist XML injection in `render_plist()`": **pre-existing
  code, not touched by this change.** Logged for a separate hardening task
  rather than scope-creeping this plan (AGENTS.md: log discovered debt).
  Added to `tech-debt-tracker.md`.
- Testing audit's host-unit-test gaps (menu_hit_test, stats_model_parse v2,
  cost-merge fallback, formatters): the repo has a documented "no host
  harness" debt; the v2 recipe was added to `docs/testing-checklist.md`
  (Host section) and the debt entry expanded. Building the harness is a
  tracked follow-up, out of this plan's scope.

## Audit Fixes

**Fixes applied**

1. Bounded numeric narrowing in `stats_model.c` (`i32_clamp`/`i64_clamp` on
   `ct,cm,tt,tm,xu,xl,h`) — addresses Security Audit §"silent integer
   overflow on JSON double→int cast".
2. Extracted `extra_pct()` in `ui.c`, replacing the duplicated guarded
   division in the Cost and Usage-Limits cards — addresses DX Audit
   §"duplicated extra-usage percentage calculation".
3. Added `WHY`/cross-ref comments (NAV_HIST_PTS, decision #9, publisher
   history order) — addresses DX Audit §"documentation/discoverability".
4. Logged the pre-existing `render_plist` XML-escaping concern to
   `tech-debt-tracker.md` instead of fixing out-of-scope code.

**Verification checklist**

- [x] `idf.py build` clean after fixes (rc=0, 0x14fc90 ≈ 1.38 MB).
- [x] Pipeline re-verified: Claude cost all 7 keys; today $87.29 / 30d
      $623.63 / 20-day history; churn fallback still exits 3 + uncorrupted.
- [x] **Flashed to hardware 2026-05-18** (1.375 MB, hash verified). Serial
      boot: clean boot + WiFi + TLS + `upstash: HTTP 200, 700 bytes` +
      `stats: parsed v=2 ... n=6`. Live publisher emits v2; device's v1‖v2
      guard + cost parser accept it. No panic / boot-loop. (On-screen render +
      touch gestures still need a human — `docs/testing-checklist.md`.)
- [ ] (on-device) A corrupt Upstash value (e.g. `"ct":1e12`) renders a
      clamped sane number, not a negative/garbage cost.
- [ ] (on-device) Extra-usage bar identical on the Cost and Usage-Limits
      cards for the same provider (single-sourced `extra_pct`).

## Post-completion fixes (2026-05-18)

User reported the Cost-card chart rendering as "a giant unlabeled orange bar
to the right edge, no margin" instead of a chart. Two root causes, both fixed
+ reflashed (boot/v2-parse re-verified clean):

1. **`LV_CHART_POINT_NONE == INT32_MAX`**: `point_count` was a fixed 31 with
   the ~11 unused tail points set to POINT_NONE. LVGL clamps that to range-max
   and draws full-height blocks (not gaps), in a BAR chart → a solid orange
   mass on the right. Fix: `lv_chart_set_point_count(n=hist_n)` +
   `lv_chart_set_value_by_id` (no filler points ever); switched BAR→**LINE**
   sparkline (matches the original mock + user preference).
2. **Card padding**: `cost_card`/`lim_card` never zeroed the default theme
   container padding, so absolute child coords were offset and a `W-24` child
   overflowed the padded content box ("no margin / runs off the edge"). Fix:
   `lv_obj_set_style_pad_all(card, 0, 0)`.
3. Added a caption label (`cost_cap`: "N-DAY SPEND • max $X") — was
   "unlabeled". Spec + testing-checklist updated (bar→line) accordingly.

Data note: a 24-hour *cost* line is NOT possible — CodexBar's cost cache is
day-granular (no hourly $). Hourly *usage-%* history DOES exist in a separate
CodexBar file (`history/claude.json`, `accounts[..].session.entries[]`).

### Follow-up shipped 2026-05-18: 24h SESSION usage-% sparkline

Implemented + flashed + verified on hardware (`upstash HTTP 200, 727 bytes`,
`parsed v=2 n=6`, clean boot). Added the optional `ph` field (array of 0..100,
≤24, oldest→newest) to the Claude provider — additive within v2, NO version
bump (old firmware ignores it; schema is a strict superset).
- `scripts/codexbar-publish.sh`: new `PCT_MERGE_JXA` reads
  `history/claude.json`, picks the `session` window, takes the last ≤24
  samples in 24h; independent + fail-safe (missing/churned file → omit `ph`).
  `CBPUB_PCT_HISTORY` test override added.
- `firmware/main/stats_model.{h,c}`: `pct_hist[STATS_PCT_HIST_MAX=24]` +
  `pct_hist_n`; parse provider-level `ph` (clamped 0..100).
- `firmware/main/ui.c`: `lim_chart` LINE sparkline + `lim_cap` caption
  ("SESSION 24H • now N%") on the Usage-Limits card; hidden when `ph` absent.
- Schema doc + ARCHITECTURE contract (`ph?`) + spec + testing-checklist
  updated. Also: project `.gitignore` rewritten (ESP-IDF build/managed/
  sdkconfig + macOS noise; PII fixtures kept ignored) — `git status` went
  from thousands of build files to ~24 intentional changes.
Pure-cost 24h hourly remains out of scope (no hourly $ source).

### Follow-up shipped 2026-05-18: per-provider theming + inverted bars

- **Per-provider accent**: `prov_accent(id,&col)` is a table mirroring
  CodexBar's full `WidgetColors.color(for:)` palette (44 providers, keyed by
  the `UsageProvider` raw value = our `id`; e.g. Claude `0xCC7C5E`, Codex
  `0x49A3B0`, Cursor `0x00BFA5`, OpenRouter `0x6F42C1`). `bar_color()` routes
  every progress bar + both chart series (summary rows, Cost/Limits cards,
  sparklines) through it; unknown ids fall back to the pct ramp. A few
  CodexBar colors are near-black (ollama/synthetic/manus/commandcode) — kept
  verbatim per the user's "use CodexBar's colors"; flagged for a possible
  min-luminance floor if a dark provider ever displays. Initially Claude-only
  (`0xe06c4b`); broadened to the full CodexBar palette on user request.
  Build/flash/boot verified.
- **Inverted bar fill**: 0% → full, 100% → empty ("headroom remaining").
  Flag `UI_BAR_INVERT_DEFAULT` (compile-time) + `ui_set_bar_invert(bool)`
  (runtime, thread-safe) for a future captive-portal/NVS toggle. Fill only —
  bar COLOR still tracks true usage % (red = high usage). No-data bars stay
  empty. Spec + testing-checklist updated. Build/flash/boot verified
  (`HTTP 200, 772 bytes`, `parsed v=2 n=6`, clean).
- **Summary-row layout**: provider name moved onto its own line above the
  bar + %; rows are two-line. Then a **provider-icon column** added on the
  left: CodexBar logos vendored to `scripts/assets/codexbar-logos/`,
  rasterized by `scripts/gen-provider-icons.py` (rsvg-convert + Pillow) into
  hand-emitted **A8** `lv_image_dsc_t`s (`firmware/main/provider_icons.c`,
  ~29 icons, +~30 KB flash). Tinted at render with the provider accent via
  `image_recolor` (color-agnostic source). `ROW_H` 26→40→48 for the icon +
  two lines. Generated `.c` is a committed build input (like the LVGL
  fonts); regenerate with the script. Build/flash/boot verified
  (`HTTP 200, 759 bytes`, `parsed v=2 n=6`, clean; bin ~1.41 MB, 83% free).
  Dark-provider caveat (ollama/synthetic/manus) applies to icon tint too —
  faithful to CodexBar by intent; user's live providers are high-contrast.

## Decisions

- 2026-05-18 #1: Use raw passthrough (no whitelist projection). Private
  single-user device/Upstash; privacy rests on endpoint+token secrecy. Must be
  re-justified in SECURITY.md; NVS-encryption logged as hardening debt.
- 2026-05-18 #2: Cost data is **not** in `codexbar usage` output. User chose to
  parse CodexBar's internal cost cache. Publisher rolls up `days` aggregates
  only; never forwards the `files` PII map; guards on cache format version.
- 2026-05-18 #3: Cost cache is **day-granular only** — a true 24h *hourly* cost
  sparkline is not sourceable. Deliver the 30-day daily $ history (what CodexBar
  itself shows) + today big number. Real *usage-%* hourly history (session/
  weekly) IS available (`history/claude.json`) and feeds the Usage-Limits card.
  This deviates from the earlier "both 24h+30d cost" ask — see Open questions.
- 2026-05-18 #4: Cost rollup lives in the **publisher**, not `codexbar-stats.sh`
  (the latter is a stateless `codexbar usage` snapshot tool; the cache parse is
  a local-file aggregation, publisher's concern). No synthetic 24h ring needed.

## Open questions

- 24h cost view: accept the limitation (30-day daily $ only, no hourly cost),
  or surface "today's spend so far" as the only intra-day signal? Proceeding
  with 30-day daily + today big number; flagged for user review.
- Per-model breakdown (Opus/Sonnet/Haiku $) is available in the cache `days`
  map — include on the Cost card now, or defer? Deferring (placeholder) to keep
  first build scoped, matching the earlier "stub the rest" decision.

## Risks

- R1 — CodexBar cost-cache format churn (`-v1/v2/v4/v6`). Guard filename+inner
  `version`; on mismatch omit the cost block (card shows "cost unavailable"),
  never fail the whole publish.
- R2 — payload version coupling: `stats_model.c:49` rejects unknown `v`. Flash
  firmware accepting `v==1‖v==2` before the publisher emits v2.
- R3 — relaxed privacy (accepted): raw usage carries PII; cost cache `files`
  map is heavy PII and must never leave the Mac (forward aggregates only);
  unencrypted NVS → physical theft exposes identity+spend (hardening debt).
- R4 — payload size: v2 + 30-day history ≫ old ~450 B; size `body[]`, keep
  `stats_t` static; `upstash.c` oversize path fails safe.
- R5 — gesture/triple-tap regression: unchanged `DEBOUNCE_US`, disjoint
  tap/swipe thresholds, PASS-only-on-summary gate.
- R6 — font ceiling: only montserrat 12/14/18 compiled; big `$`/`%` use 18.
