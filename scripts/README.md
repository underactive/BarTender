# Host scripts

macOS tooling for the **macOS half** of the BarTender pipeline (Prompt 1 + 2).
These scripts read CodexBar + Pi Agent state locally and publish a reduced
v2 JSON payload to a private Upstash key; the ESP32 firmware in
`firmware/main/` is the read-only consumer. See `../README.md` for the
end-to-end flow and `../docs/SECURITY.md` for the privacy model that governs
what may cross Upstash.

Layout:

| Path | Role |
|------|------|
| `*.sh` (this directory) | User-facing CLIs: stats, publish, setup, uninstall |
| [`build/`](build/) | Asset generators, serial screenshot tools, dev tests |
| [`lib/`](lib/) | Shared Python helpers (`_stats_history.py`) imported by stats scripts |

## User-facing (repo root `scripts/`)

```sh
./scripts/codexbar-stats.sh
./scripts/codexbar-stats.sh --json
./scripts/codexbar-publish.sh --once
./scripts/preflight.sh
./scripts/openrouter-stats.sh --check   # OpenRouter token pull (auth probe)
```

## Build / dev (`scripts/build/`)

```sh
python3 scripts/build/gen-provider-icons.py
python3 scripts/build/gen-boot-splash.py
python3 scripts/build/screenshot.py
./scripts/build/boot-capture.sh
python3 scripts/build/pipeline-check.py   # end-to-end pipeline diagnosis
```

Build inputs live under `scripts/build/assets/` (provider SVGs, boot splash PNG, LEMONMILK font for LVGL conversion).

### Pipeline diagnostics

`pipeline-check.py` verifies every stage host→Upstash without touching device
or secrets: config/Keychain/launchd schedule, CodexBar cost-cache scan
freshness vs actual session-file activity (detects the stalled-scanner case
where tiles freeze), publish-log freshness/cadence/failures, live Upstash
payload age + byte integrity, and per-provider movement between runs (local
snapshot diff). Read-only; exit code is scriptable (`0` healthy, `1`
failures, `2` warnings only); `--json` for machine output, `--skip-producer`
to skip the local `codexbar-stats.sh --json` run.
