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
| [`lib/`](lib/) | Shared Python helpers (`_stats_history.py`, non-executing `_trpc_extract.py`) imported by stats scripts |

## User-facing (repo root `scripts/`)

```sh
./scripts/codexbar-stats.sh
./scripts/codexbar-stats.sh --json
./scripts/codexbar-publish.sh --once
./scripts/codexbar-publish.sh --status           # job state, resolved curl, readiness
./scripts/codexbar-publish.sh --check-transport  # read-only curl probe (diagnose HTTP 000)
./scripts/preflight.sh
./scripts/openrouter-stats.sh --check   # OpenRouter token pull (auth probe)
./scripts/deepseek-stats.sh --check     # DeepSeek token/spend pull (auth probe)
```

`--check-transport` runs the exact curl binary and flags the publisher uses
against a **read-only** `GET /get/<key>` (nothing is written), then prints the
resolved curl path/version, `http_code`, curl exit class, `ssl_verify_result`,
`time_total`, proxy-env names, and a verdict locating any failure
(publisher-local / launchd-environment / curl-stack / http-auth). Publisher exit
codes: `0` published, `3` no-fresh-data skip, `4` producer failure, `5`
config/token, `6` transport failure (`HTTP 000`), `7` HTTP failure (see
`--help`).

### MiMo daily history

`scripts/mimo-stats.sh` uses dated detail/list records for daily tokens and
spend. If detail retrieval fails, it retains cached daily history; undated
summary totals are used only to check connectivity, never as today's usage.
`--check` never creates or modifies the history file. Offline regression check:
`python3 scripts/build/test-mimo-history.py`.

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
to skip the local `codexbar-stats.sh --json` run. The publisher automatically
rotates `publish.log` above `CBPUB_LOG_MAX_BYTES` (default 8 MiB) and keeps
three gzip archives. When a publish fails it now records curl's own exit code
and `curl: (N) …` line, so `pipeline-check.py` surfaces the transport **class**
(dns/connect/timeout/tls/local-curl-config) in `publish-failures`, decodes
launchd's last exit code, checks the installed plist (stale install / curl
divergence via `launchd-env`), reports whether the job is cycling at all
(`log-freshness`), guards the token shape, and flags second-writer / clock-skew
(`writer-consistency`). Use `scripts/opencodego-stats.sh --debug` or
`OPENCODE_GO_DEBUG=1` for OpenCode Go pagination diagnostics. The helper reads
its cookie from Keychain account `opencodego-session`; tests use
`OPENCODE_GO_COOKIE`, `OPENCODE_GO_HISTORY`, and a curl shim. Set
`OPENCODE_GO_WORKSPACE` only when the configured workspace changes. For a
local temporary diagnosis, `OPENCODE_GO_DUMP` writes a mode-0600 raw page to
`$TMPDIR` and prints only its path; never commit that file.

### Host tests

Hermetic stdlib-`unittest` tests for the publisher transport path and the
checker logic live under `scripts/lib/tests/` (fake `security`/`curl`/producer
shims + a loopback mock sink; never touch the real Keychain, network, or
launchd). Qwen projection and OpenCode Go parser tests also run through this
harness:

```sh
python3 -m unittest discover -s scripts/lib/tests -p 'test_*.py'
# or individually:
python3 scripts/lib/tests/test_publish_transport.py
python3 scripts/lib/tests/test_pipeline_check.py
python3 scripts/lib/tests/test_qwen_projection.py -v
python3 scripts/lib/tests/test_opencodego_parser.py -v
```
