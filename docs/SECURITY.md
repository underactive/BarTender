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
exception**, made acceptable by constraining what crosses the boundary:

- **Whitelisted projection only.** `codexbar-stats.sh --json` *builds* the
  payload field-by-field (`id`, `ok`, usage `%`, reset hint) — it does not
  filter the raw CodexBar JSON. PII (account emails, `loginMethod`,
  `identity`, org names) and `$` cost figures and credentials are therefore
  structurally impossible to leak, not merely stripped. A verification step
  greps the payload to assert this.
- **Credentials never transit and never rest in the repo.** The Upstash
  **write** token lives in the macOS Keychain (service `codexbar-toy`),
  passed to `curl` via a `0600 -K` config (never argv/log/plist). The device
  holds a *separate* **read-only** token.
- **Minimal blast radius.** Only one Redis key of non-sensitive aggregate
  usage percentages is exposed; a leak reveals "how busy this user's AI
  plans are," not identity, content, or secrets.
- **Fail-safe.** A transient local failure must not overwrite the store with
  empty/all-error data (publish is skipped — the toy keeps last-known-good).

Any change that would widen the payload (e.g. adding `$` spend, account
identifiers) MUST update this section and re-justify the boundary.

## Device boundary: the ESP32 toy (Prompt 3)

- **Read-only by capability.** The device is provisioned with the Upstash
  **read-only** token, never the write token. Extraction from a physically
  accessible desk object yields only the ability to read data that is
  already non-sensitive (usage %). It cannot tamper with the store.
- **No secrets in the firmware or repo.** WiFi creds + URL + read token are
  entered via the first-boot captive portal and stored in NVS. They are
  never compiled in, never committed (`firmware/secrets.h` is not used).
- **NVS is unencrypted.** Threat is physical possession of the board only.
  Flash/NVS encryption is a documented hardening follow-up, out of POC scope.
- **SoftAP exposure is bounded.** The provisioning AP is WPA2 (device-unique
  password shown only on the local TFT), runs only while unprovisioned, and
  the device reboots out of AP mode immediately on form submit. The captive
  HTTP form is plaintext but reachable only by a station that already has the
  WPA2 PSK, on local RF.
- **TLS trust** uses the bundled Mozilla CA store (`esp_crt_bundle`), not a
  pinned certificate — robust to Upstash cert rotation without a reflash.

## Sensitive files

The system should warn (not block) if operations touch:
- `.env`, `.env.*`
- Files matching `*secret*`, `*credential*`, `*token*`
- CI/CD configuration (`.github/workflows/`, `.gitlab-ci.yml`)
- Package lockfiles (flag for human review)
