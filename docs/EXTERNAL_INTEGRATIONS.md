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
  `codexbar serve` does **not** cache (rejected). The reduced projection maps
  generic `credits.remaining` to `cost.cr` (integer cents) when available; it
  also reduces Moonshot's `Balance: $…` display string and DeepSeek's leading
  `$…` balance/reset string to `cost.cr`, while OpenRouter retains its dedicated
  balance source. See memory `codexbar-cli-behavior`.

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
- **Environment / hostname gating:** outbound HTTPS POST of a whitelisted,
  privacy-reduced payload only (usage % + reset hints + reduced spend/token
  rollups; no PII/credentials/raw sessions/project paths — see
  `docs/SECURITY.md`). `MOCK_SINK_URL` redirects to a local sink for tests.
- **Key env vars / CLI flags:** config keys `UPSTASH_REST_URL`, `UPSTASH_KEY`,
  `PUBLISH_INTERVAL`, `MOCK_SINK_URL`; test overrides `CBPUB_CONFIG`,
  `CBPUB_LOG_DIR`, `CBPUB_KC_SERVICE`; subcommands `--once/--set-token/
  --set-cursor-session/--set-cursor-session-clipboard/--install/--uninstall/
  --status/--print-plist`.
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

## Ramp Router dashboard API

- **What:** router.ramp.com account balance and usage rollups for the `ramp`
  provider tile (CodexBar has no Ramp Router hook). Reverse-engineered from
  the dashboard frontend (verified 2026-08-11).
- **Loaded via:** `scripts/ramp-stats.sh` — the Keychain session cookie
  (service `codexbar-toy`, account `ramp-session`; stored with
  `codexbar-publish.sh --set-ramp-cookie[-clipboard]`) mints a ~15-minute JWT
  from `GET /api/auth/token`, which Bearer-authorizes
  `GET /client/billing` (balance snapshot: `remaining_credit_usd`,
  `total_credits_usd`) and `GET /client/usage/dashboard` (30-day
  `summary.spend_usd` / `summary.total_tokens` + per-day `series`).
  `merge-ramp.js` appends the sanitized generic `cost` block and drops the
  device-hidden `opencode` row to stay within the firmware's 12-provider cap.
- **Lifecycle:** invoked per publish cycle with a 30 s timeout. Fail-soft:
  a dead cookie re-emits the last good snapshot from
  `~/.config/codexbar-toy/ramp-cache.json`; no credential and no cache exits 3
  and the publisher publishes without Ramp.
- **Privacy:** only aggregate cents/tokens cross Upstash — no model names,
  request contents, API keys, or account identifiers (`docs/SECURITY.md`).
- **Gotchas:** the session cookie is httpOnly (must be copied from DevTools,
  not `document.cookie`); the JWT expires in ~15 min so it is minted fresh
  every run; `ramp-stats.sh --check` probes auth without publishing.

## Pi Agent local state

- **What:** Pi Agent CLI harness local session history and custom provider/model
  configuration. This is a Mac-local source used to add `pi` as a first-class
  provider row in the same payload consumed by the ESP32.
- **Loaded via:** `scripts/pi-agent-stats.sh`, invoked fail-soft from
  `scripts/codexbar-publish.sh --once`. The helper scans
  `~/.pi/agent/sessions/**/*.jsonl` for assistant `usage` objects and reads
  `~/.pi/agent/models.json` only as the local Pi Agent config surface.
- **Lifecycle:** read once per publish cycle. No daemon and no network call.
  Missing directories, malformed rows, or absent usage data make the helper
  exit non-zero; the publisher logs the skip and continues publishing the
  non-Pi payload.
- **Environment / hostname gating:** local macOS files only. The published
  boundary is reduced to `{id:"pi", ok:true, p, pi:{ts,tt,ps,pt,h}}`: today's
  spend/tokens, max daily spend/tokens, and 30-day daily spend history. Raw prompts,
  command/session trees, cwd/project paths, model names, response IDs, and
  provider credentials never leave the Mac.
- **Key env vars / CLI flags:** `PI_AGENT_HOME`, `PI_AGENT_SESSIONS_DIR`,
  `PI_AGENT_MODELS_FILE`, `PYTHON3`; `--help`.
- **Gotchas:** Do not confuse Pi Agent sessions with CodexBar's unrelated
  `pi-sessions-v*.json` cost-cache files, which are used only to roll up Codex
  cost. Local/free Pi backends may have token usage with `$0.00` spend; the Pi
  row can still render token counts while the spend graph is flat.

## clawd-tank board layer (vendored)

- **What:** the proven ESP-IDF board bring-up for the Freenove ESP32-S3 2.8"
  (FNK0104) — ILI9341+LVGL 9, FT6336 touch, shared I2C, NVS pattern.
- **Loaded via:** **copied verbatim** into `firmware/main/` from
  `/Users/esison/Development/projects/hardware/clawd-tank/firmware/main/`
  (`board_config.h`, `i2c_bus.*`, `display.*` byte-identical; `touch.*`
  retargeted to `APP_EVT_TOUCH`). clawd-tank is NOT a build dependency — this
  is a vendoring snapshot, not a live link.
- **Lifecycle:** static source; re-vendor by re-copying if clawd-tank's board
  layer improves. No runtime coupling.
- **Env/gating:** ESP-IDF 5.3+, target `esp32s3`, `CONFIG_BOARD_FREENOVE_S3_28`.
- **Key files:** `firmware/main/board_config.h` (FNK0104 pins/geometry).
- **Gotchas:** ILI9341 orientation/mirror flags are a clawd-tank bring-up TODO
  (flip `BOARD_LCD_MIRROR_X/Y` together if the image is rotated). LVGL/
  esp_lcd_* are managed components (auto-fetched), pinned to clawd-tank's
  versions in `firmware/main/idf_component.yml`.

## Upstash (device read path — Prompt 3)

- **What:** the ESP32 reads the same key the publisher writes.
- **Loaded via:** `firmware/main/upstash.c` — `esp_http_client` +
  `esp_crt_bundle` (Mozilla CA, no pinning); bearer token from NVS
  (provisioned via captive portal, never in source).
- **Credential:** the Upstash **read-only** token (distinct from the Mac's
  write token). Least privilege: cannot modify the store.
- **Gotchas:** response is the double-encoded `{"result":"<json>"}` envelope —
  `stats_model.c` parses envelope then inner. Confirmed against live bytes.
