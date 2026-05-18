# Plan: CodexBar stats → Upstash publisher

- **Started:** 2026-05-17
- **Status:** Active
- **Objective** — periodically publish a minimal, non-sensitive CodexBar usage
  JSON to Upstash Redis (REST) via a macOS `launchd` job, so the Prompt 3
  ESP32 toy can read it with a read-only token.
- **Changes**
  - `scripts/codexbar-stats.sh` — added `--json` mode (`CBAR_MODE=json` branch
    in the shared JXA program): emits a whitelisted compact payload.
  - `scripts/codexbar-publish.sh` — new: publish cycle + Keychain token mgmt +
    launchd install/uninstall/status + `--print-plist`.
  - `launchd/com.codexbar-toy.publish.plist.template` — new: committed, no
    secrets, placeholders filled at `--install`.
  - Docs: this plan, `PLANS.md`, `EXTERNAL_INTEGRATIONS.md`, `SECURITY.md`,
    `QUALITY_SCORE.md`, `README.md`, product spec.
- **Dependencies** — Prompt 1 (`codexbar-stats.sh`) is the data source; `--json`
  must land before the publisher is useful. Real end-to-end publish depends on
  the user provisioning Upstash + `--set-token`.
- **Risks / open questions**
  - Keychain access under launchd (user GUI session — expected OK; locked
    keychain would prompt). Documented in EXTERNAL_INTEGRATIONS.
  - Upstash GET envelope `{"result":"<json-string>"}` ⇒ ESP32 double-parses
    (carried to Prompt 3).
  - `nc` flavor for the mock sink in tests.

## Context

The ESP32 is standalone IoT and cannot reach the Mac directly, so usage stats
must transit an intermediary. `docs/SECURITY.md` forbids intermediaries /
data leakage; resolved by publishing only a whitelisted projection (usage % +
reset hints; never emails/identity/credentials/$) and codifying the bounded
exception in SECURITY.md. The write token lives in the macOS Keychain; the
device gets a separate read-only token. Zero third-party deps (base-macOS
`security`/`curl`/`launchctl`/`awk`/`date` + the Prompt 1 script).

## Steps

- [x] `--json` mode in `codexbar-stats.sh` (reuse `CBAR_MODE` switch)
- [x] `codexbar-publish.sh` (cycle, Keychain, launchd, `--print-plist`)
- [x] launchd plist template (no secrets)
- [x] Verification: skip-on-empty, Keychain round-trip, mock-sink publish
      (request+auth+body+no-PII), `--print-plist` lint, real install/uninstall
- [x] Docs: PLANS index, EXTERNAL_INTEGRATIONS, SECURITY, QUALITY_SCORE,
      README, product spec
- [x] User provisioning + **real Upstash round-trip (2026-05-18)**: live
      `--once` → HTTP 200; GET round-trip double-parsed fresh data, no PII;
      `--install` → launchd autonomous run `last exit code = 0`, republishing
      every 300 s to the user's Upstash DB (`<db>.upstash.io`) key `codexbar`.
- [ ] Post-implementation audit (docs/AUDIT.md) → move plan to completed/

## Decisions

- 2026-05-17: Store = Upstash Redis REST (least infra, ideal for ESP32 GET).
- 2026-05-17: Secret = macOS Keychain (no plaintext on disk; SECURITY.md
  "secure vaults"). `--set-token` uses `security`'s own prompt so the token
  never enters argv.
- 2026-05-17: curl auth via 0600 `-K` config, not `-H` argv (no token in `ps`).
- 2026-05-17: Skip-publish when `codexbar-stats.sh --json` rc≠0 — never
  overwrite a good store value on a transient local failure.
- 2026-05-17: `--json` is a whitelist projection, not a filter of the full
  CodexBar JSON — fields are added explicitly, so PII cannot leak by default.

## Open questions

- Final Upstash key name (default `codexbar`) — confirm when provisioning.
- Whether the toy should also show `$` spend (currently excluded; future opt-in).

## Verification performed

Hermetic, mock-first (real Upstash deferred to user):
`--json` valid/whitelisted (PII grep clean, 276 B); live `--json` ~2 s exit 0;
skip-on-empty (exit 3, no POST); Keychain round-trip (token absent from
log/argv); mock-sink publish (`POST /set/<key>`, `Authorization: Bearer …`,
minimal body, no PII); `--print-plist` `plutil -lint` OK + placeholders
filled + no secrets; real `--install` → loaded → `--uninstall` → removed,
no trace; zero third-party deps.
