# Host scripts

macOS tooling for the BarTender pipeline. Layout:

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
```

## Build / dev (`scripts/build/`)

```sh
python3 scripts/build/gen-provider-icons.py
python3 scripts/build/gen-boot-splash.py
python3 scripts/build/screenshot.py
./scripts/build/boot-capture.sh
```

Build inputs live under `scripts/build/assets/` (provider SVGs, boot splash PNG, LEMONMILK font for LVGL conversion).
