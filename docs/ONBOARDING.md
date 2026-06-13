# BarTender — Onboarding Guide

A 3-stage pipeline that pipes CodexBar AI-provider usage and cost stats from your Mac
to an ESP32-S3 desk toy.

```
  macOS (laptop)                          cloud               device
  ┌──────────────────────┐   launchd     ┌──────────┐  HTTPS  ┌──────────────┐
  │ codexbar (CLI)        │   every 5m    │ Upstash  │  GET    │ ESP32-S3     │
  │  └ codexbar-stats.sh │──┐            │ Redis    │◀────────│ (FNK0104)    │
  │     --json (Prompt 1)│  │ POST /set  │ key      │ read-   │ WiFi+TLS     │
  │ codexbar-publish.sh  │──┴───────────▶│ codexbar │ only    │ → ILI9341 UI │
  │  (Prompt 2)          │   write token └──────────┘  token  │ (Prompt 3)   │
  └──────────────────────┘                               └──────────────┘
       reads local data         v2 usage+cost JSON              read-only consumer
```

---

## What It Does

1. **Read locally** — `scripts/codexbar-stats.sh` reads AI-provider usage data from
   CodexBar's local state on your Mac.
2. **Publish to cloud** — `scripts/codexbar-publish.sh` merges cost rollups and publishes
   a privacy-reduced JSON payload to a private Upstash Redis key.
3. **Display on device** — firmware reads the key over HTTPS and renders usage stats on a
   Freenove ESP32-S3 2.8" LCD screen.

**This is a private single-user system.** Everything is per-your-account. No PII, no
credentials, and no raw project paths ever leave your Mac.

---

## Dependencies Quick Reference

### Host (macOS)

| Dependency | Why Needed | How to Check | Install If Missing |
|------------|-----------|-------------|-------------------|
| **CodexBar** (`codexbar` CLI) | Primary data source for AI-provider usage stats | `codexbar --version` | [steipete/CodexBar](https://github.com/steipete/CodexBar) |
| **ESP-IDF 5.3+** (`idf.py`) | Build & flash firmware | `idf.py --version` | [ESP-IDF Install Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/) |
| **Python 3** | Icon generation, screenshot protocol, Pi Agent stats | `python3 --version` | `brew install python3` |
| **Pillow** (Python) | SVG-to-A8-alpha rasterization for provider icons | `python3 -c "import PIL"` | `pip3 install Pillow` |
| **pyserial** (Python) | Firmware screenshot protocol over USB serial | `python3 -c "import serial"` | `pip3 install pyserial` |
| **rsvg-convert** | SVG-to-PNG conversion for icons | `rsvg-convert --version` | `brew install librsvg` |

> **Note:** The core publish pipeline (`codexbar-publish.sh --once`) has **zero**
> third-party Python dependencies. Only the optional tooling (`gen-provider-icons.py`,
> `screenshot.py`) requires Python packages.

### Cloud

| Service | Why Needed | Setup |
|---------|-----------|-------|
| **Upstash Redis** | Bridges Mac → ESP32 (no direct connectivity). Holds one key with usage JSON. | Create a free DB at [console.upstash.com](https://console.upstash.com), grab the REST URL, and generate a write token + read-only token. |

### Device (ESP32-S3)

| Component | Spec | Notes |
|-----------|------|-------|
| **Board** | Freenove ESP32-S3 2.8" (FNK0104) | Connected via USB-C |
| **Display** | ILI9341 SPI, 240×320 portrait | LVGL 9 renders on top |
| **Touch** | FT6336G capacitive, shared I2C | No physical buttons — navigation is touch-only |
| **LED** | WS2812 on GPIO 42 | RMT driver |
| **Flash** | 16 MB | Single ~8 MB factory app partition (`0x7F0000` in partitions.csv) |

---

## Directory Layout

| Directory | Role |
|-----------|------|
| `scripts/` | User-facing host CLIs (stats, publish, preflight) |
| `scripts/build/` | Asset generators, screenshot tools, build inputs |
| `scripts/build/assets/codexbar-logos/` | Canonical SVG inputs for provider icon generation |
| `scripts/lib/` | Shared Python helpers for stats scripts |
| `firmware/` | ESP-IDF project root: build config, partitions, managed deps |
| `firmware/main/` | First-party firmware runtime: boot, NVS, WiFi, fetch, parsing, UI, provisioning |
| `launchd/` | LaunchAgent plist template (no secrets — placeholders filled by `--install`) |
| `docs/` | Product specs, plans, security rules, generated payload schema |
| `docs/generated/` | `codexbar-payload.schema.json` — the end-to-end payload contract |
| `docs/product-specs/` | User-facing specs (esp32-toy, publish-to-upstash, claude-cost-menu) |
| `docs/references/` | Local/private capture fixtures (`.gitignore`d, never committed) |
| `.rpiv/guidance/` | AI agent architecture guidance (shadow tree) |

---

## Data Flow — Detailed

### Stage 1: Local Stats (`scripts/codexbar-stats.sh`)

```
codexbar-stats.sh
  │
  ├─ reads ~/.codexbar/config.json  → enabled providers list
  ├─ spawns parallel `codexbar usage --provider <id> --source <cli|auto>` per provider
  ├─ watchdog kills anything > CBAR_TIMEOUT (default 45s)
  │
  ├─ --json → v2 JSON:
  │     { v: 2, ts: "<publisher-iso8601>", providers: [
  │         { id: "claude", ok: true, p: 45.3, pr: "resets in 2h",
  │           s: 72.1, sr: "resets in 1d",
  │           cost: { xu: 1250, xl: 5000 } }  // extra-usage overage
  │       ]
  │     }
  │
  └─ exits 3 when zero providers returned real data (used by publisher as skip signal)
```

**Privacy:** `codexbar-stats.sh` never reads `accountEmail`, `loginMethod`, or `identity`.
PII is structurally absent by construction.

### Stage 2: Publish to Upstash (`scripts/codexbar-publish.sh`)

```
codexbar-publish.sh --once
  │
  ├─ runs codexbar-stats.sh --json → base payload
  │
  ├─ reads CodexBar local cost cache (~/Library/Caches/CodexBar/cost-usage/claude-v*.json)
  │     → merges 30-day per-day spend history into each provider's cost.h[]
  │     → NEVER reads the files map (private project paths)
  │
  ├─ optionally merges Pi Agent stats from ~/.pi/agent/sessions/**/*.jsonl
  │     → reduced to max daily spend, max daily tokens, 30-day history
  │
  ├─ optionally merges Cursor daily token rollup
  │
  ├─ read write token from macOS Keychain (service: codexbar-toy, account: publish)
  │     → passed to curl via 0600 temp -K file (never in argv/log/plist)
  │
  ├─ POST {UPSTASH_REST_URL}/set/{UPSTASH_KEY}
  │     body: JSON payload with bearer token in header
  │
  └─ skips write on empty (preserves last good — transient faults don't blank the toy)
```

**Config:** `~/.config/codexbar-toy/config` (KEY=VALUE lines):
```
UPSTASH_REST_URL=https://<your-db>.upstash.io
UPSTASH_KEY=codexbar
PUBLISH_INTERVAL=300        # seconds (default 300 / 5 min)
```

**Concurrency guard:** A `$LOG_DIR/.publish.lock` dir — second instance prints "skip:
already running" and exits 0.

### Stage 3: Firmware (`firmware/`)

```
ESP32-S3 boot
  │
  ├─ NVS init → config_store (WiFi, Upstash, display settings)
  ├─ MODE GATE:
  │     if (no Upstash OR no WiFi): provision_start() → captive SoftAP
  │     else: normal mode
  │
  ├─ net_wifi_start_multi() → background scan + autoconnect (≤5 networks, MRU order)
  ├─ fetch_task_start() → poll loop:
  │     1. GET {url}/get/{key} with read-only bearer token
  │     2. Parse {"result":"<json>"} envelope → inner JSON → flat stats_t struct
  │     3. ui_set_stats(&st, now_ms()) → mutex-protected copy to UI task
  │     4. Sleep FETCH_INTERVAL (60s) / FETCH_RETRY_S on error
  │
  └─ ui_task → LVGL rendering (all LVGL calls on one thread):
       summary: per-provider row (bar + %, logo, id), status line
       cost page: big $, token counts, 30-day bar graph
       usage limits: session %, weekly %, extra usage bars, 24h sparkline
       nav: swipe ↔ tap ↔ long-press (touch-only)
```

---

## Security Model

| Boundary | Detail |
|----------|--------|
| **Write token** | macOS Keychain (service `codexbar-toy`, account `publish`); never in git, argv, logs, or plist |
| **Read token** | Device NVS (unencrypted — physical theft risk only); limited to read-only Upstash operations |
| **Payload (v2)** | Usage %, reset hints, extra-usage $, spend rollups, 30-day spend history. No PII, no credentials, no raw sessions |
| **Fail-safe** | Empty/failed publish is SKIPPED — the store keeps its last good value |
| **Least privilege** | Device holds read-only token; extraction yields only usage %, not cost or identity |

See [`docs/SECURITY.md`](SECURITY.md) for the full threat model.

---

## Quick-Start Commands

```bash
# See your stats locally
./scripts/codexbar-stats.sh

# Emit JSON for the publisher
./scripts/codexbar-stats.sh --json

# One publish cycle
./scripts/codexbar-publish.sh --once

# Build firmware
cd firmware && idf.py set-target esp32s3 && idf.py build

# Flash + monitor (connect via USB-C first)
cd firmware && idf.py -p /dev/tty.usbmodem* flash monitor

# Regenerate provider icons from SVG assets
python3 scripts/build/gen-provider-icons.py

# Capture firmware screen via USB
python3 scripts/build/screenshot.py
```

---

## Troubleshooting

### `codexbar not found`
Install CodexBar from [steipete/CodexBar](https://github.com/steipete/CodexBar) or set `CODEXBAR_BIN`.

### Publish skips silently
Check the launchd log: `cat ~/Library/Logs/codexbar-toy/publish.log`.
Run manually: `./scripts/codexbar-publish.sh --once` to see stdout.

### Firmware won't connect to WiFi
Long-press on the summary screen → captive portal opens. Add network via phone/laptop
at `http://192.168.4.1/`. The device remembers ≤5 networks in MRU order.

### Upstash returns 401/403
Write token may have been revoked. Run `./scripts/codexbar-publish.sh --set-token` to
replace it. For the device, re-provision via the captive portal.

### ESP-IDF not found
Export the ESP-IDF path: `export PATH=$HOME/esp/esp-idf:$PATH` or use `source
$HOME/esp/esp-idf/export.sh`. See [ESP-IDF Install Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/).

---

## Payload Schema

The contract between publisher and firmware is in [`docs/generated/codexbar-payload.schema.json`](generated/codexbar-payload.schema.json).

**Wire format:**
```
GET {upstash_url}/get/{key}
  → { "result": "{\"v\":2,\"ts\":\"...\",\"providers\":[...]}" }
```

**v2 payload (inner):**
```json
{
  "v": 2,
  "ts": "<publisher-iso8601>",
  "providers": [
    {
      "id": "claude",
      "ok": true,
      "p": 45.3,
      "pr": "resets in 2h",
      "s": 72.1,
      "sr": "resets in 1d",
      "cost": {
        "ct": 1247,
        "cm": 18500,
        "tt": 1250000,
        "tm": 8750000,
        "xu": 1250,
        "xl": 5000,
        "h": [120, 135, 0, 0, 200, ...]
      },
      "ph": [10, 15, 45, 80, 60, ...]
    }
  ]
}
```

- `v`: schema version (1 or 2 accepted; v3+ rejected loudly)
- `ts`: publisher ISO-8601 timestamp
- `providers[]`: per-provider row with usage %, cost, and optional history
- `cost.h[]`: 30-day per-day spend history in cents
- `ph[]`: ~24h session usage-% history (Claude only)

---

## Related Docs

- [`ARCHITECTURE.md`](../ARCHITECTURE.md) — system overview and design decisions
- [`docs/SECURITY.md`](SECURITY.md) — threat model and security rules
- [`docs/EXTERNAL_INTEGRATIONS.md`](EXTERNAL_INTEGRATIONS.md) — CodexBar, Upstash, Pi Agent details
- [`firmware/README.md`](../firmware/README.md) — firmware build, flash, and provisioning
- [`.rpiv/guidance/`](../.rpiv/guidance/) — AI agent architecture guidance
