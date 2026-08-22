# Security

Security boundaries must be clearly defined and enforced, especially for
systems that process external input or integrate with third-party services.

## Threat model

1. **External input is untrusted.** Data from APIs, user input, third-party
   services, or file uploads may contain injection attempts, malformed
   payloads, or malicious content. All external data is treated as untrusted
   strings until validated.

2. **User data may be sensitive.** The system must not leak user data to
   unintended destinations. Integrations should use official APIs and
   authenticated channels — no intermediary services.

3. **Credentials are user-managed.** The system does not store or manage API
   keys or secrets directly. External services handle their own auth. The
   system only references credentials via environment variables or secure
   vaults.

## Rules

- **No `eval` or dynamic code execution on external input.** Ever.
- **No shell interpolation of external data.** Use array-form exec, not
  string concatenation, when external data appears near shell invocations.
- **File paths from external input are validated** against an allowed root
  directory. No path traversal.
- **Operations are read-only by default.** Destructive or write operations
  require explicit opt-in from the user.
- **Temporary resources are disposable.** They are created in scoped
  directories and cleaned up after each operation.
- **No network calls triggered by external data.** If external input contains
  a URL, that URL is displayed or logged — not fetched.

## Project exception: the CodexBar → ESP32 publish path (Prompt 2)

Threat-model rule 2 says "no intermediary services." The desktop-toy roadmap
*requires* one: the ESP32 is standalone IoT and cannot reach the Mac directly,
so usage stats transit Upstash Redis. This is a **deliberate, bounded
exception**.

### v1 (historical): structural minimization

`codexbar-stats.sh --json` built the payload field-by-field (`id`, `ok`,
usage `%`, reset hint). PII and `$` were *structurally impossible* to leak.
This is the model `ARCHITECTURE.md` decision #2 describes; it is **superseded
at v2** but retained here for context.

### v2 (current): deliberately RELAXED single-user channel

The owner explicitly chose to put **real cost data** on the toy. The privacy
model changed from "structural minimization" to **"the Upstash endpoint and
read token are private to one user."** This was an informed decision for a
personal desk object, recorded here as the mandated re-justification.

**What now crosses the boundary (v2 payload):** usage `%` + reset hints,
extra-usage `$` (cents), prepaid account balances for OpenRouter/MiMo,
Moonshot/DeepSeek (reduced to cents from CodexBar's provider display data),
DeepSeek aggregate per-day token counts and USD-cent spend rollups from the
platform.deepseek.com dashboard API via `deepseek-stats.sh` (the responses
carry per-model breakdowns, but model names are summed away during aggregation
and never reach the payload; no request contents or account identifiers), and
Ramp Router (cents + aggregate spend/token rollups from the router.ramp.com
dashboard API via `ramp-stats.sh`; no model names, request contents, or
account identifiers),
Claude/Codex total spend and token rollups, a 30-day per-day spend history where
available, aggregate Moonshot/Qwen Cloud daily token counts derived from local
Pi Agent sessions (attributed by provider id only — model names, prompts, and
session paths are still never projected; this widens the Pi reduction from
purely aggregate to per-provider attribution for those two ids), and — for Pi
Agent — max daily spend,
max daily tokens, and a 30-day daily spend history reduced from local Pi Agent
session usage. Account email / `identity` / `loginMethod` are still **not
projected** (`codexbar-stats.sh` simply never reads them; they are absent by
construction, not by redaction). Pi Agent prompts, command/session trees,
cwd/project paths, model names, response IDs, and provider credentials are also
not projected; `pi-agent-stats.sh` reads only usage/cost counters from local
session JSONL rows.

**Why this is acceptable here, and only here:**

- **Single-user, private intermediary.** One Upstash database, one Redis key,
  a read token held only by the owner's own device. The channel's
  confidentiality now rests on **endpoint + token secrecy**, not on the
  payload being non-sensitive. A leak of the endpoint+token would reveal this
  user's AI spend and rough 30-day spend shape — accepted by the owner.
- **Raw local source files never leave the Mac.** Cost is rolled up by
  `codexbar-publish.sh` from CodexBar's local caches
  (`~/Library/Caches/CodexBar/cost-usage/claude-v*.json`, Codex cache files).
  Those caches may contain private project-path maps; the publisher reads only
  aggregate day maps and forwards rolled-up numbers. Pi Agent is reduced by
  `scripts/pi-agent-stats.sh` from `~/.pi/agent/sessions/**/*.jsonl`; it reads
  assistant `usage` counters and timestamps, then emits only max spend/tokens
  plus daily spend history. Raw prompts, cwd/project paths, model/provider
  identifiers, response IDs, and credentials are structurally never emitted.
  This local-source minimization is the hard, non-negotiable boundary that
  remains.
- **Cache/session-format churn is fail-safe.** The cache schema version churns
  (`claude-v1/-v2/...`) and Pi Agent session rows can vary by release. On any
  unrecognized local shape the publisher omits that reduced block/provider and
  publishes the rest — it never aborts and never emits a half-parsed/garbage
  cost figure.
- **Credentials never transit and never rest in the repo.** The Upstash
  **write** token lives in the macOS Keychain (service `codexbar-toy`),
  passed to `curl` via a `0600 -K` config (never argv/log/plist). Provider
  credentials sit beside it under the same service — including the OpenRouter
  management key (account `openrouter-key`) and the DeepSeek platform
  `userToken` (account `deepseek-token`), which `openrouter-stats.sh` and
  `deepseek-stats.sh` send via an in-process `urllib` header rather than
  `curl`, so neither appears in argv where `ps` could read it. The DeepSeek
  value is a full **dashboard session** credential rather than a scoped API
  key, so it is never echoed to the log or plist and `--set-deepseek-token`
  reads it with a hidden `read -rs`. The device
  holds a *separate* **read-only** token. The capture fixtures under
  `docs/references/` (account email, spend) are `.gitignore`d — they must
  never be committed if this repo is ever versioned.
- **Fail-safe.** A transient local failure must not overwrite the store with
  empty/all-error data (publish is skipped — the toy keeps last-known-good);
  a PID-aware single-flight lock prevents overlapping cycles and recovers locks
  left by an interrupted publisher without removing a live owner's lock.

**Residual risk (accepted, with mitigation owed):** the device NVS is
unencrypted (see below). Under v1 a stolen device leaked only "how busy."
Under v2 it leaks the owner's identity-adjacent **spend and 30-day spend
shape**. NVS encryption is therefore upgraded from "nice-to-have" to a
**tracked hardening item** (`docs/exec-plans/tech-debt-tracker.md`).

Any FURTHER widening (e.g. per-model breakdown, the cache `files` map,
account identifiers) MUST update this section and re-justify again.

## Device boundary: the ESP32 toy (Prompt 3)

- **Read-only by capability.** The device is provisioned with the Upstash
  **read-only** token, never the write token. Extraction from a physically
  accessible desk object yields only the ability to read data that is
  already non-sensitive (usage %). It cannot tamper with the store.
- **No secrets in the firmware or repo.** WiFi creds + URL + read token are
  entered via the captive portal and stored in NVS. They are never compiled
  in, never committed (`firmware/secrets.h` is not used). The Upstash token
  is **never re-rendered into any HTML response**: the add-network form omits
  the Upstash fields entirely (it cannot echo a value it never emits).
- **NVS is unencrypted.** Threat is physical possession of the board only.
  **Risk elevated at payload v2:** the toy caches the owner's Claude spend +
  30-day spend shape. **Scope widened further:** the device now remembers up
  to **5** WiFi PSKs (a single `cbtoy/wnets` blob) instead of one, so a
  physically stolen board exposes more home/work network passphrases. This
  reinforces — does not change — the conclusion that flash/NVS encryption is
  a **tracked** hardening item (`docs/exec-plans/tech-debt-tracker.md`).
- **SoftAP exposure is bounded.** The provisioning AP is WPA2 (device-unique
  password shown only on the local TFT). It now also runs on demand for the
  non-destructive "add a network" flow (long-press / self-heal set a one-shot
  flag, consumed clear-before-act so a power loss cannot boot-loop into it),
  and the device reboots out of AP mode immediately on form submit. The
  captive HTTP form is plaintext but reachable only by a station that already
  has the WPA2 PSK, on local RF.
- **TLS trust** uses the ISRG Root X2 public CA anchor in
  `firmware/main/upstash_roots.h` with mbedTLS's normal hostname and chain
  validation. This is **not** a leaf-cert pin and never disables verification.
  It is a compatibility fallback for ESP-IDF 5.3.2's compact bundle callback:
  it forces the valid EC X2 path instead of the X1 cross-sign path that the
  device cannot verify. Trust is narrower than the prior Mozilla bundle: an
  Upstash switch away from ISRG requires a firmware update. Remove this
  fallback after upgrading ESP-IDF to a bundle implementation that validates
  that hierarchy correctly.

## Sensitive files

The system should warn (not block) if operations touch:
- `.env`, `.env.*`
- Files matching `*secret*`, `*credential*`, `*token*`
- CI/CD configuration (`.github/workflows/`, `.gitlab-ci.yml`)
- Package lockfiles (flag for human review)
