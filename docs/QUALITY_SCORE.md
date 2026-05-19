# Quality Score

Tracks the current quality grade of each domain and architectural layer.
Updated as domains are built. Agents and humans use this to prioritize
cleanup and investment.

## Grading scale

| Grade | Meaning                                                    |
|-------|-----------------------------------------------------------|
| A     | Well-tested, documented, agent-legible, no known debt      |
| B     | Functional and tested, minor gaps in docs or edge cases    |
| C     | Works but has known debt, missing tests, or unclear naming |
| D     | Fragile, undertested, or structurally problematic          |
| F     | Broken or placeholder only                                 |

## Domain grades

| Domain            | Grade | Notes                                          | Last reviewed |
|-------------------|-------|-------------------------------------------------|---------------|
| `codexbar-stats` (read) | A- | v2 projection (usage % + extra-usage $ cents, `v:1`→`v:2`); PII still never projected by construction; live-verified. Whitelist relaxed per SECURITY.md v2 | 2026-05-18 |
| `codexbar-publish` (Upstash) | B | v2: merges Claude cost from CodexBar's **undocumented** local cache (churn-guarded, fail-safe) + single-flight lock. Verified end-to-end vs live cache + sanitized fixtures. Debt: cache-format coupling, no automated harness | 2026-05-18 |
| `firmware` (ESP32-S3) | B | v2 parser (v1‖v2 + cost block) + swipe-nav menu/submenu/Cost+Limits cards + lv_chart history; `idf.py build` clean (1.50 MB). Multi-WiFi: Upstash decoupled from WiFi; ≤5-network MRU/LRU NVS blob (atomic, validated, fail-safe); net_wifi manager task scans-on-disconnect → strongest-in-range select (8 dB stickiness) → MRU-promote, event handler kept a pure signaller so the lock-free `s_connected` invariant holds; legacy single-SSID auto-migrates (no erase). Triple-tap/self-heal now non-destructive (one-shot `fprov`, clear-before-act). Grade stays **B**: build-clean and the concurrency/atomicity/fail-safe risks were designed out, but this is a large new surface with NO host harness and on-device verification of the 6-scenario script still pending user flash — net neutral vs. the UX/robustness gain. Debt: NVS encryption (scope widened to ≤5 PSKs), no host harness, on-device verification pending | 2026-05-18 |

## Cross-cutting grades

| Concern           | Grade | Notes                                          | Last reviewed |
|-------------------|-------|-------------------------------------------------|---------------|
| <!-- e.g., Logging --> | <!-- e.g., B --> | <!-- e.g., Structured JSON, missing correlation IDs --> | <!-- e.g., 2026-03-30 --> |

## Process

- Review and update grades when a domain ships or changes significantly.
- A domain at grade C or below should have an entry in
  [tech-debt-tracker.md](exec-plans/tech-debt-tracker.md).
- Background cleanup tasks target the lowest-graded domains first.
