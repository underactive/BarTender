# External Integrations

Document each third-party service, SDK, model bundle, or asset the project
depends on. Add a new `##` section per integration. Keep entries factual —
this is the doc an agent reads before touching anything that calls out of
process.

## CodexBar CLI

- **What:** [steipete/CodexBar](https://github.com/steipete/CodexBar) — macOS
  menu-bar app + `codexbar` CLI reporting AI-provider usage. The data source
  for the whole project.
- **Loaded via:** `scripts/codexbar-stats.sh` shells out to the `codexbar`
  binary (`$CODEXBAR_BIN`, default `command -v codexbar`,
  `/opt/homebrew/bin/codexbar`). Enabled providers from `~/.codexbar/config.json`.
- **Lifecycle:** invoked per run; one parallel single-provider call per enabled
  provider; results parsed by an embedded JXA program. No daemon.
- **Environment / hostname gating:** local macOS only. `codexbar usage`
  performs provider network fetches (server-side usage %). `--source cli`
  reads local caches; `auto` may scrape web dashboards.
- **Key env vars / CLI flags:** `CODEXBAR_BIN`, `CODEXBAR_CONFIG`,
  `CBAR_CLI_PROVIDERS` (default `codex`), `CBAR_TIMEOUT`; `--json`, `--all`.
- **Gotchas:** exit code is non-zero if *any* provider fails but JSON is still
  valid (JSON is authoritative); `--provider all` returns ~40 providers;
  `codexbar serve` does **not** cache (rejected). See memory `codexbar-cli-behavior`.

## Upstash Redis (REST)

- **What:** serverless Redis with an HTTPS REST API. Holds one key (default
  `codexbar`) containing the minimal usage JSON. Publisher writes; the Prompt 3
  ESP32 reads.
- **Loaded via:** `scripts/codexbar-publish.sh` — `curl POST <REST_URL>/set/<key>`.
  Non-secret config (`UPSTASH_REST_URL`, `UPSTASH_KEY`) from
  `~/.config/codexbar-toy/config`; **write token from the macOS Keychain**
  (service `codexbar-toy`, account `publish`), passed via a 0600 `curl -K`
  config — never argv/log/plist.
- **Lifecycle:** invoked by the `com.codexbar-toy.publish` launchd LaunchAgent
  every `PUBLISH_INTERVAL` s (default 300) and at load. Skips the write when
  there is no fresh data (store keeps last good).
- **Environment / hostname gating:** outbound HTTPS POST of a **whitelisted,
  non-sensitive** payload only (usage % + reset hints; no PII/credentials/$ —
  see `docs/SECURITY.md`). `MOCK_SINK_URL` redirects to a local sink for tests.
- **Key env vars / CLI flags:** config keys `UPSTASH_REST_URL`, `UPSTASH_KEY`,
  `PUBLISH_INTERVAL`, `MOCK_SINK_URL`; test overrides `CBPUB_CONFIG`,
  `CBPUB_LOG_DIR`, `CBPUB_KC_SERVICE`; subcommands `--once/--set-token/
  --install/--uninstall/--status/--print-plist`.
- **Gotchas:** launchd has a sparse env — the installer bakes `CODEXBAR_BIN`
  and `PATH` into the plist. Upstash `GET /get/<key>` wraps the value:
  `{"result":"<json-string>"}` → the ESP32 must parse the envelope then the
  inner JSON (Prompt 3 contract). Free-tier request limits apply.

### Required files / credentials

| File / credential | Role | Source |
|-------------------|------|--------|
| Upstash REST URL | non-secret endpoint, in `~/.config/codexbar-toy/config` | console.upstash.com (Redis DB → REST) |
| Upstash **write** token | publish auth; macOS Keychain `codexbar-toy/publish` | Upstash DB tokens (write) |
| Upstash **read-only** token | ESP32 read auth (Prompt 3) | Upstash DB tokens (read-only) |
| `~/.codexbar/config.json` | enabled-provider list (read-only) | CodexBar app |
