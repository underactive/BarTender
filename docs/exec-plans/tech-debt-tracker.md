# Tech Debt Tracker

Known technical debt, tracked as inventory. Items here should be addressed
by targeted cleanup tasks on a regular cadence — not accumulated for a
"big refactor."

## Format

```
### <short title>
- **Domain:** which domain is affected
- **Grade impact:** what quality grade this drags down
- **Severity:** low | medium | high
- **Added:** YYYY-MM-DD
- **Notes:** context for why this exists and what fixing looks like
```

## Active debt

### NVS encryption (risk elevated at payload v2)

- **Domain:** firmware
- **Grade impact:** firmware B+ → B
- **Severity:** medium
- **Added:** 2026-05-18
- **Notes:** Device NVS is unencrypted. At v2 the toy caches the owner's
  Claude spend + 30-day spend shape, so physical theft now leaks
  identity-adjacent financial data, not just "how busy." **Scope widened
  (2026-05-18, wifi-lru-persistent-upstash):** the device now stores up to
  **5** WiFi PSKs (the `cbtoy/wnets` blob) instead of one, so a stolen board
  also exposes more home/work passphrases. Severity unchanged (still
  physical-possession-only) but the blast radius grew. Fix: enable ESP-IDF
  flash/NVS encryption + secure boot. See `docs/SECURITY.md` §"Device boundary".

### CodexBar cost-cache format coupling

- **Domain:** codexbar-publish
- **Grade impact:** codexbar-publish B+ → B
- **Severity:** medium
- **Added:** 2026-05-18
- **Notes:** The Claude cost rollup parses CodexBar's **undocumented** internal
  cache (`~/Library/Caches/CodexBar/cost-usage/claude-v*.json`,
  `days[d][m]=[input,cacheRead,cacheCreate,output,costNanos,rows,rows]`). The
  filename version churns (`-v1/-v2/-v4/-v6` observed). Guarded: unknown shape
  → publish usage-only (fail-safe), but a silent CodexBar format change
  degrades the Cost card with no alert. Fix: pin/snapshot the schema in CI, or
  petition CodexBar for a stable `cost --json` CLI.

### No host test harness for the v2 parser/pipeline

- **Domain:** firmware / codexbar-publish
- **Grade impact:** firmware B
- **Severity:** low
- **Added:** 2026-05-18
- **Notes:** `stats_model_parse` (now v1‖v2 + cost block) and the publisher
  cost-merge JXA were verified manually against live data + sanitized
  fixtures, not in an automated harness. The `firmware/test/host/` seam should
  compile `stats_model.c` against host cJSON and assert the v2 fixture; the
  JXA merge needs a mock-cache test. **Extended (2026-05-18):** the new
  `config_store` WiFi-list logic (LRU rotate/evict, blob validation,
  same-SSID-in-place update, legacy migration) is pure, host-testable C with
  no hardware deps and is currently only on-device-verifiable — a strong
  candidate for the same `firmware/test/host/` seam.

### Plist template lacks XML escaping (pre-existing)

- **Domain:** codexbar-publish
- **Grade impact:** none currently (paths are user-controlled, `plutil -lint`
  catches gross corruption)
- **Severity:** low
- **Added:** 2026-05-18 (surfaced by the claude-cost-menu audit; the code
  predates this change and was not modified by it)
- **Notes:** `render_plist()` substitutes `$LOG`/`$CODEXBAR_BIN`/`$PATH` into
  the launchd plist via `${t//__X__/$VAR}` with no XML entity escaping. A path
  containing `& < > " '` would produce a malformed plist. Fix: XML-escape the
  substituted values (or template via `plutil`/`PlistBuddy`). Tracked
  separately to avoid scope-creeping the cost-menu plan.

### 24h hourly cost not sourceable; per-model breakdown deferred

- **Domain:** firmware (Cost card)
- **Grade impact:** none (scoped deviation, documented)
- **Severity:** low
- **Added:** 2026-05-18
- **Notes:** CodexBar's cost cache is day-granular, so the Cost card's "24h"
  view degrades to a "today's spend" big number + the real 30-day daily bar
  chart (exec-plan `claude-cost-menu` Decision #3). Per-model (Opus/Sonnet/
  Haiku) `$` split exists in the cache `days` map but is intentionally not
  surfaced yet.

### WiFi blob path: dense-RF scan truncation + on-stack scratch

- **Domain:** firmware (net_wifi / config_store)
- **Grade impact:** firmware B (no change; masked, not eliminated)
- **Severity:** medium
- **Added:** 2026-05-19 (found during first on-hardware bring-up of the
  wifi-lru + nav work — never exercised on device before)
- **Notes:** Two related shortcuts taken to fix on-device regressions:
  (1) `SCAN_MAX_AP=24` is too small for RF-dense sites (32–38 APs observed);
  `esp_wifi_scan_get_ap_records` truncates unordered, so the target SSID can
  fall out of the read window. Masked by a direct-connect-to-MRU fallback,
  which only rescues the *single* MRU network — multi-network roaming in a
  dense location is still degraded. Proper fix: per-SSID directed scan
  (`wifi_scan_config_t.ssid`) or a larger/dynamic record buffer.
  (2) `config_store_wifi_*` put a ~490 B `wifi_creds_t` on the *caller's*
  stack; this overflowed the 4 KB httpd task and panicked (bogus
  `xTaskPriorityDisinherit` assert). Fixed by enlarging the httpd (8 KB) and
  wifi_mgr (6 KB) task stacks rather than moving the buffer off-stack —
  fragile if a new small-stack caller of the blob API appears. Proper fix:
  heap- or static-scratch the blob buffer so config_store is stack-frugal
  regardless of caller.

## Resolved debt

(none)

## Process

- When you discover tech debt during a task, add it here rather than fixing
  it inline (unless the fix is trivial and scoped to your current change).
- Cleanup tasks should reference the specific item they resolve.
- Move resolved items to the "Resolved" section with the date and PR/commit.
