# AGENTS.md

Instructions for AI coding agents working in **BarTender** — a macOS → Upstash → ESP32-S3 pipeline that displays CodexBar AI usage on a desk toy.

## Agent directives

1. **Never commit to git until explicitly directed.** Stage and describe changes if helpful, but do not run `git commit`, `git push`, or amend history unless the user asks.
2. **Always verify work before declaring done.** Run the narrowest checks that prove the change (see [Verification](#verification)). If a check cannot run in this environment (hardware, Keychain, Upstash), say what was verified and what the user should run locally.
3. **Keep relevant docs in sync when you touch their domain.** Use the [Doc maintenance matrix](#doc-maintenance-matrix) — do not leave contract, security, or user-visible behavior undocumented.

Additional expectations:

- **Minimize scope** — smallest correct change; match existing naming and patterns (`docs/REPO_CONVENTIONS.md`, `docs/CODE_STYLE.md`).
- **Respect privacy** — publish only the reduced payload shape; never widen what crosses Upstash without reading `docs/SECURITY.md`.
- **Treat external input as untrusted** — validate at system edges (`stats_model.c`, `upstash.c`, `config_store.c`, host JSON producers).

---

## System at a glance

```
macOS scripts/          launchd/              Upstash Redis           firmware/main/
codexbar-stats.sh  →  codexbar-publish.sh  →  (REST GET/SET)  →  fetch → stats_model → ui
```

Authoritative architecture: [`ARCHITECTURE.md`](ARCHITECTURE.md). Layer-specific agent guidance (shadow tree): [`.rpiv/guidance/`](.rpiv/guidance/).

---

## Repository map

### Top level

| Path | Role |
|------|------|
| [`firmware/`](firmware/) | ESP-IDF project — device runtime, LVGL UI, WiFi, Upstash consumer |
| [`scripts/`](scripts/) | macOS host pipeline, asset generation, host↔device utilities |
| [`docs/`](docs/) | Specs, policies, contracts, onboarding, exec plans |
| [`launchd/`](launchd/) | LaunchAgent template for scheduled publish |
| [`.rpiv/guidance/`](.rpiv/guidance/) | AI-oriented architecture notes (prefer updating when behavior changes) |
| [`.rpiv/artifacts/`](.rpiv/artifacts/) | Agent-generated plans/research (ephemeral; not product source) |
| [`README.md`](README.md) | User-facing quick start and install |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | System design, domains, payload overview |

Do **not** edit `firmware/build/`, `firmware/managed_components/`, or `scripts/__pycache__/`.

---

### Source: `scripts/` (macOS host)

See [`scripts/README.md`](scripts/README.md) for layout. User-facing CLIs live at `scripts/`; build tooling under `scripts/build/`; shared Python under `scripts/lib/`.

| File | Purpose |
|------|---------|
| `codexbar-stats.sh` | Read CodexBar locally; human report + `--json` payload (producer) |
| `codexbar-publish.sh` | Merge cost/history; publish v2 JSON to Upstash; Keychain + launchd install |
| `cursor-stats.sh` | Optional Cursor session stats for publish merge |
| `pi-agent-stats.sh` | Pi Agent usage rollups for publish merge |
| `lmstudio-stats.sh` | LM Studio stats integration |
| `preflight.sh` | Dependency check, guided setup, optional `--install` / `--flash` |
| `uninstall.sh` | Remove launchd job and local install artifacts |

**`scripts/build/`** (generators, dev tools)

| File | Purpose |
|------|---------|
| `gen-provider-icons.py` | SVG → `firmware/main/provider_icons.{c,h}` |
| `gen-boot-splash.py` | Boot splash asset generation |
| `screenshot.py` | Host client for firmware serial screenshot protocol |
| `boot-capture.sh` | Boot-time capture helper |
| `test-lmstudio-phase1.py` | LM Studio integration test script |
| `assets/codexbar-logos/` | Canonical SVG/logo inputs for icon generation |
| `assets/boot-splash.png` | Boot splash source image |

**`scripts/lib/`**

| File | Purpose |
|------|---------|
| `_stats_history.py` | Shared history helpers (imported by `cursor-stats.sh`, `lmstudio-stats.sh`) |

Config on the user's machine (not in repo): `~/.config/codexbar-toy/`; secrets in macOS Keychain per `docs/SECURITY.md`.

---

### Source: `firmware/` (ESP32-S3)

| Path | Purpose |
|------|---------|
| `firmware/main/` | Application component — all product C source |
| `firmware/test/<module>/` | Host-runnable unit tests (`Makefile`, `./runtests`) |
| `firmware/CMakeLists.txt`, `partitions.csv`, `sdkconfig*` | IDF project config |
| `firmware/README.md` | Build/flash prerequisites |

#### `firmware/main/` modules

| Module | Files | Concern |
|--------|-------|---------|
| Entry / orchestration | `main.c`, `app_event.h` | Boot, provisioning gate, task wiring |
| Config / persistence | `config_store.{c,h}`, `wifi_lru.h` | NVS: WiFi LRU, Upstash URL, setup flags |
| Payload model | `stats_model.{c,h}` | Parse/clamp Upstash JSON → `stats_t` |
| Network / cloud | `net_wifi.{c,h}`, `upstash.{c,h}`, `fetch.{c,h}` | WiFi, HTTPS GET, fetch loop |
| Provisioning | `provision.{c,h}` | Captive portal setup |
| UI | `ui.{c,h}`, `ui_render_core.c`, `ui_render_card.c`, `ui_render_summary.c`, `ui_format.c`, `ui_screensaver.c`, `ui_internal.h` | LVGL (single UI task) |
| Display / input / LED | `display.{c,h}`, `touch.{c,h}`, `i2c_bus.{c,h}`, `led.{c,h}` | Hardware adapters |
| Branding / assets | `provider_icons.{c,h}`, `provider_colors.h`, `font_lemonmilk_*.c`, `boot_splash.{c,h}` | Generated or embedded assets |
| Diagnostics | `screenshot.{c,h}`, `board_config.h` | Screenshot protocol, pin/board defs |
| Build | `CMakeLists.txt`, `Kconfig.projbuild`, `idf_component.yml` | Component registration |

#### `firmware/test/` (off-device)

| Suite | Tests |
|-------|--------|
| `test/stats_model/` | Payload parsing (uses `docs/references/*.sample.json`) |
| `test/config_store/` | WiFi LRU / config logic |
| `test/ui_format/` | UI formatting helpers |

---

### Documentation: `docs/`

#### Policies and how-to

| Doc | When to read / update |
|-----|------------------------|
| [`ONBOARDING.md`](docs/ONBOARDING.md) | Setup flow, deps, first flash — update if install steps change |
| [`SECURITY.md`](docs/SECURITY.md) | Tokens, Keychain, NVS, payload privacy — **read before** secret or publish changes |
| [`RELIABILITY.md`](docs/RELIABILITY.md) | Failure modes, backoff, degraded behavior |
| [`REPO_CONVENTIONS.md`](docs/REPO_CONVENTIONS.md) | Naming, boundaries, tests, logging |
| [`CODE_STYLE.md`](docs/CODE_STYLE.md) | Formatting and C/shell style |
| [`EXTERNAL_INTEGRATIONS.md`](docs/EXTERNAL_INTEGRATIONS.md) | CodexBar, Upstash, Cursor, Pi Agent |
| [`testing-checklist.md`](docs/testing-checklist.md) | Manual QA behaviors — extend when adding user-visible behavior |
| [`POST_IMPLEMENTATION.md`](docs/POST_IMPLEMENTATION.md) | Post-ship checklist |

#### Design and product

| Doc | Purpose |
|-----|---------|
| [`DESIGN.md`](docs/DESIGN.md), [`design-docs/`](docs/design-docs/) | Design principles and beliefs |
| [`PRODUCT_SENSE.md`](docs/PRODUCT_SENSE.md) | Product framing |
| [`product-specs/`](docs/product-specs/) | User stories and acceptance criteria per feature |
| [`product-specs/index.md`](docs/product-specs/index.md) | Spec catalog |

#### Contracts and references

| Doc | Purpose |
|-----|---------|
| [`generated/codexbar-payload.schema.json`](docs/generated/codexbar-payload.schema.json) | **Authoritative** Upstash payload JSON Schema |
| [`generated/README.md`](docs/generated/README.md) | Schema index and sync rules |
| [`references/*.sample.json`](docs/references/) | Local fixtures for parser tests (**gitignored patterns** — do not commit secrets) |

#### Planning and history (reference)

| Doc | Purpose |
|-----|---------|
| [`PLANS.md`](docs/PLANS.md) | Planning index |
| [`exec-plans/active/`](docs/exec-plans/active/) | In-flight execution plans |
| [`exec-plans/completed/`](docs/exec-plans/completed/) | Completed plan archive |
| [`exec-plans/tech-debt-tracker.md`](docs/exec-plans/tech-debt-tracker.md) | Known debt |
| [`HISTORY.md`](docs/HISTORY.md), [`AUDIT.md`](docs/AUDIT.md), [`QUALITY_SCORE.md`](docs/QUALITY_SCORE.md) | Historical / audit context |

---

### Other

| Path | Purpose |
|------|---------|
| [`launchd/com.codexbar-toy.publish.plist.template`](launchd/com.codexbar-toy.publish.plist.template) | Rendered by `codexbar-publish.sh --install` |

---

## Doc maintenance matrix

When you change… | Also update…
---|---
Payload fields or JSON shape | `docs/generated/codexbar-payload.schema.json`, `scripts/codexbar-stats.sh` / `codexbar-publish.sh`, `firmware/main/stats_model.*`, `firmware/test/stats_model/`, relevant `docs/product-specs/*.md`, `README.md` if user-visible |
Host publish schedule, Keychain, config paths | `docs/SECURITY.md`, `docs/product-specs/publish-to-upstash.md`, `.rpiv/guidance/scripts/architecture.md` |
Firmware UI pages, gestures, provisioning | `docs/product-specs/esp32-toy.md` (and feature-specific specs), `docs/testing-checklist.md`, `.rpiv/guidance/firmware/main/architecture.md` |
WiFi LRU or NVS layout | `firmware/test/config_store/`, `docs/RELIABILITY.md` if behavior changes |
New provider icon | `scripts/build/assets/codexbar-logos/`, run `python3 scripts/build/gen-provider-icons.py`, commit generated `provider_icons.*` |
System boundaries or pipeline stages | `ARCHITECTURE.md`, `.rpiv/guidance/architecture.md` |

---

## Verification

Run what applies to your change. Prefer automated checks over assumptions.

### Host scripts

```sh
# Syntax / help (no CodexBar required for --help)
./scripts/codexbar-stats.sh --help
./scripts/codexbar-publish.sh --help

# Full pipeline (requires CodexBar + local config)
./scripts/codexbar-stats.sh
./scripts/codexbar-stats.sh --json | python3 -m json.tool
./scripts/codexbar-publish.sh --once   # needs Upstash + Keychain token
./scripts/preflight.sh                 # dependency audit
```

### Firmware — off-device unit tests

```sh
cd firmware/test/stats_model && ./runtests
cd firmware/test/config_store && ./runtests
cd firmware/test/ui_format && ./runtests
```

### Firmware — build

```sh
cd firmware && idf.py build
```

Requires ESP-IDF 5.3+ on PATH. Build must be clean (`-Werror`).

### Firmware — on device (user)

```sh
cd firmware && idf.py -p /dev/tty.usbmodem* flash monitor
```

Use [`docs/testing-checklist.md`](docs/testing-checklist.md) for behavior-level QA after UI or provisioning changes.

### Cross-cutting: payload contract

After producer or consumer changes, confirm samples still parse and schema matches:

1. Regenerate or hand-edit `docs/generated/codexbar-payload.schema.json`
2. `cd firmware/test/stats_model && ./runtests`
3. Optionally `./scripts/codexbar-stats.sh --json` and compare to schema

---

## Common commands (quick reference)

See `README.md` §Quick start for the full command reference. The same
commands are also listed in each script's `--help` output.

---

## Where to start by task type

| Task | Start here |
|------|------------|
| Payload / provider data | `scripts/codexbar-stats.sh`, `docs/generated/codexbar-payload.schema.json`, `firmware/main/stats_model.c` |
| Publishing / secrets | `scripts/codexbar-publish.sh`, `docs/SECURITY.md` |
| Display / touch / pages | `firmware/main/ui*.c`, `docs/product-specs/esp32-toy.md` |
| WiFi / provisioning | `firmware/main/provision.c`, `config_store.c`, `net_wifi.c` |
| New feature behavior | Matching `docs/product-specs/*.md` + `docs/testing-checklist.md` |
| Repo-wide orientation | `ARCHITECTURE.md`, `.rpiv/guidance/architecture.md` |
