# {{PROJECT_NAME}} Architecture

## System overview

A 3-stage pipeline that puts CodexBar AI-usage stats on a standalone desk toy.
The ESP32 cannot reach the Mac directly, so an intermediary (Upstash) bridges
them; only a whitelisted, non-sensitive payload crosses it.

```
  macOS (laptop)                         cloud            device
  ┌────────────────────┐   launchd     ┌─────────┐  HTTPS  ┌──────────────┐
  │ codexbar (CLI)      │   every 5m    │ Upstash │  GET    │ ESP32-S3     │
  │  └ codexbar-stats.sh│──┐            │ Redis   │◀────────│ (FNK0104)    │
  │     --json (Prompt1)│  │ POST /set  │  key    │ read-   │ WiFi+TLS     │
  │ codexbar-publish.sh │──┴───────────▶│ codexbar│ only    │ → ILI9341 UI │
  │  (Prompt 2)         │   write token └─────────┘  token  │ (Prompt 3)   │
  └────────────────────┘                              └──────────────┘
       reads local data        non-PII JSON only        read-only consumer
```

Data contract (stable across stages):
`GET {url}/get/{key}` → `{"result":"<escaped-json>"}` →
`{v,ts,providers:[{id,ok,p?,pr?,s?,sr?}]}`.

## Domain layers

<!-- Define your dependency layers. The key principle: dependency flows in
     one direction only. Adapt these layers to your architecture. -->

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

<!-- List every bounded domain in your system. Update this table when adding
     or removing domains. -->

| Domain            | Purpose                                           | Status      |
|-------------------|---------------------------------------------------|-------------|
| `codexbar-stats` (`scripts/codexbar-stats.sh`) | Read CodexBar locally; text report + `--json` whitelist | Implemented |
| `codexbar-publish` (`scripts/codexbar-publish.sh`) | Publish JSON to Upstash on a launchd schedule | Implemented |
| `firmware` (`firmware/`) | ESP32-S3 reads Upstash, renders on ILI9341; captive provisioning | Implemented (POC, user-flashed) |

## Key design decisions

<!-- Record architectural decisions that agents need to understand.
     Focus on "why" — agents can read the code to learn "what".
     Number them sequentially and never remove entries (mark superseded instead). -->

1. **JSON is authoritative, not exit codes.** CodexBar exits non-zero if any
   provider fails but still emits valid JSON; every consumer trusts the parsed
   payload, not the process/HTTP status. (Discovered in Prompt 1.)
2. **Whitelisted projection, not filtering.** `--json` builds the published
   object field-by-field, so PII/credentials/$ cannot leak by construction —
   this is what makes the Upstash intermediary acceptable (see SECURITY.md).
3. **Skip-on-empty.** The publisher never overwrites a good store value with
   empty/all-error data; the toy keeps last-known-good through transient faults.
4. **Vendored board layer.** Prompt 3 copies clawd-tank's proven ILI9341/LVGL/
   FT6336/I2C/NVS modules verbatim rather than rewriting board bring-up; only
   WiFi/HTTPS/UI/provisioning is new code.
5. **Least privilege at the edge.** The device holds a read-only Upstash token;
   even if extracted it can only read already-non-sensitive data.
6. **TLS via Mozilla CA bundle**, not cert pinning — survives Upstash cert
   rotation without a firmware reflash.
