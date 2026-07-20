# BarTender Architecture

> **Stage glossary** — docs in this repo refer to the three pipeline stages as
> "Prompt 1/2/3". They map to:
> - **Prompt 1** = `codexbar-stats.sh` (local stats reader)
> - **Prompt 2** = `codexbar-publish.sh` (Upstash publisher)
> - **Prompt 3** = `firmware/` (ESP32-S3 device firmware)
>
> The names are historical project-artifact handles; just use the file paths.

## System overview

A 3-stage pipeline that puts CodexBar AI-usage **and cost** stats on a
standalone desk toy. The ESP32 cannot reach the Mac directly, so an
intermediary (Upstash) bridges them. As of payload v2 the channel is a
**deliberately relaxed, private single-user link**: usage %, reset hints,
extra-usage $, and rolled-up Claude spend/tokens/30-day history cross it.
Privacy rests on Upstash endpoint + read-token secrecy, not on structural
payload minimization (see SECURITY.md; NVS-encryption is logged tech debt).

```
  macOS (laptop)                         cloud            device
  ┌────────────────────┐   launchd     ┌─────────┐  HTTPS  ┌──────────────┐
  │ codexbar (CLI)      │   every 5m    │ Upstash │  GET    │ ESP32-S3     │
  │  └ codexbar-stats.sh│──┐            │ Redis   │◀────────│ (FNK0104)    │
  │     --json (Prompt1)│  │ POST /set  │  key    │ read-   │ WiFi+TLS     │
  │ codexbar-publish.sh │──┴───────────▶│ codexbar│ only    │ → ILI9341 UI │
  │  (Prompt 2)         │   write token └─────────┘  token  │ (Prompt 3)   │
  └────────────────────┘                              └──────────────┘
       reads local data      v2 usage+cost JSON       read-only consumer
```

Data contract (stable across stages; full JSON Schema in
[docs/generated/codexbar-payload.schema.json](docs/generated/codexbar-payload.schema.json)):
`GET {url}/get/{key}` → `{"result":"<escaped-json>"}` →
`{v,ts,providers:[{id,ok,p?,pr?,s?,sr?,cost?,ph?}]}` where the optional
`cost = {xu?,xl?,ct?,cm?,tt?,tm?,cr?,h?}` (cents / token counts; `cr` is a
prepaid balance from OpenRouter, generic CodexBar `credits.remaining`, or the
reduced Moonshot/DeepSeek balance display strings) and `ph` = ~24h
session usage-% history (0..100; Claude only).
**v1** is the pre-cost shape `{id,ok,p?,pr?,s?,sr?}`; v2 is a strict superset
and the firmware accepts both (`v1‖v2`).

## Domain layers

| Layer        | Responsibility                                    | May depend on        |
|--------------|---------------------------------------------------|----------------------|
| **Types**    | Shared interfaces, enums, schemas                 | Nothing              |
| **Config**   | Environment, feature flags, configuration         | Types                |
| **Repository** | Data access, file I/O, persistence              | Types, Config        |
| **Service**  | Business logic, orchestration, parsing            | Types, Config, Repo  |
| **Runtime**  | CLI entry points, HTTP handlers, process lifecycle | All above            |
| **UI**       | Terminal output, formatting, interactive prompts  | All above            |

Cross-cutting concerns (logging, telemetry, error handling) should be injected
via a shared interface. Domains should not import cross-cutting code directly.

## Domains

| Domain            | Purpose                                           | Status      |
|-------------------|---------------------------------------------------|-------------|
| `codexbar-stats` (`scripts/codexbar-stats.sh`) | Read CodexBar locally; text report + `--json` v2 (usage % + extra-usage $) | Implemented |
| `codexbar-publish` (`scripts/codexbar-publish.sh`) | Merge Claude cost from CodexBar's local cost cache; publish v2 to Upstash on a launchd schedule | Implemented |
| `firmware` (`firmware/`) | ESP32-S3 reads Upstash, scrollable summary + tap-cycle Cost/Usage pages on ILI9341; captive provisioning; remembers ≤5 WiFi nets (LRU) and scans+autoconnects when relocated, Upstash decoupled from WiFi | Implemented (POC, user-flashed) |

## Key design decisions

1. **JSON is authoritative, not exit codes.** CodexBar exits non-zero if any
   provider fails but still emits valid JSON; every consumer trusts the parsed
   payload, not the process/HTTP status. (Discovered in Prompt 1.)
2. **Whitelisted projection, not filtering.** *(Superseded by #7 at v2.)*
   `--json` builds the published object field-by-field, so PII/credentials/$
   cannot leak by construction — this is what made the Upstash intermediary
   acceptable at v1 (see SECURITY.md).
3. **Skip-on-empty.** The publisher never overwrites a good store value with
   empty/all-error data; the toy keeps last-known-good through transient faults.
4. **Vendored board layer.** Prompt 3 copies clawd-tank's proven ILI9341/LVGL/
   FT6336/I2C/NVS modules verbatim rather than rewriting board bring-up; only
   WiFi/HTTPS/UI/provisioning is new code.
5. **Least privilege at the edge.** The device holds a read-only Upstash token;
   even if extracted it can only read already-non-sensitive data.
6. **TLS via Mozilla CA bundle**, not cert pinning — survives Upstash cert
   rotation without a firmware reflash.
7. **Relaxed single-user channel at v2.** The user opted to carry real cost
   data on a private device. Privacy now rests on Upstash endpoint + read-token
   secrecy, not structural minimization. The cost source is CodexBar's local
   cache (`~/Library/Caches/CodexBar/cost-usage/claude-v*.json`), built from
   `~/.claude` transcripts — NOT `codexbar usage` output. The publisher reads
   ONLY its aggregate `days` map and forwards rolled-up numbers; the cache's
   `files` map (private project paths) is never read. Cache schema churns
   (`-v1/-v2/...`) → guarded, degrades to usage-only. Residual risk: unencrypted
   NVS exposes identity+spend on physical theft (logged tech debt).
8. **Firmware owns a stable flat schema.** The MCU parser is NOT coupled to
   CodexBar's nested, version-churning `usage.*` JSON; the publisher normalizes
   to the firmware-owned v2 contract. v2 ⊃ v1, so the version guard is
   trivially safe (old payloads parse; unknown future `v` is rejected loudly).
9. **UI navigation state lives in `ui.c`.** The nav state machine is
   mutex-protected `st`, mutated via `ui_handle_input()` (called on the fetch
   task) — chosen over a separate nav module to keep all LVGL calls on
   `ui_task`. *(Superseded at the nav redesign: the original 4-level swipe
   menu→submenu→card was replaced by a 2-state machine — a **scrollable
   summary** ⇄ a per-provider **page**. Vertical swipe page-scrolls the
   fixed `row_*[]` slots through an `st.scroll` offset; tap a row opens its
   Cost page; tap cycles Cost↔Limit; swipe-left returns. `fetch.c` acts ONLY
   when `ui_handle_input` returns PASS, which now means a LONG-PRESS on the
   summary → open the add-network portal. Tap-to-refresh was dropped as a
   deliberate consequence; the 60 s device poll (FETCH_INTERVAL_S) keeps pages live.)*
10. **Upstash and WiFi are decoupled; WiFi is an MRU ≤5-network roaming
   store.** WiFi creds live in one versioned NVS blob (`cbtoy/wnets`,
   `wifi_creds_t`), written with a single `nvs_set_blob`+commit so an LRU
   rotate/evict is atomic — chosen over indexed keys (torn multi-key write)
   and over `esp_wifi`'s built-in single-cred store (no multi-net/LRU/scan).
   Upstash URL/key/token stay as independent string keys so changing WiFi
   never re-prompts for the token and a corrupt WiFi blob can't endanger it
   (blob validated on read; failure ⇒ "zero networks", never erase/brick).
   A net_wifi-owned manager task scans **only while disconnected** (the toy
   polls every 60 s (FETCH_INTERVAL_S; the macOS publisher writes every 300 s), so it never needs to roam mid-link; relocating just
   produces a real disconnect → scan) and the WiFi event handler is a **pure
   signaller** — this is what preserves the lock-free `s_connected`
   single-writer invariant (#net_wifi.c:12-15) while adding scan/select/MRU
   state. The summary long-press and the 180 s self-heal are
   **non-destructive**: they set a one-shot `fprov` flag (consumed
   clear-before-act ⇒ no boot-loop) that opens an *add-network* portal
   keeping all creds. WPA2-PSK only by user decision (open/Enterprise/
   web-login venues out of scope).
