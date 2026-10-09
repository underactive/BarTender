# Reliability

Requirements and practices for keeping the system reliable.

## Failure modes to handle

| Failure | Likelihood | Mitigation |
|---------|------------|------------|
| Upstash unreachable / TLS / DNS error | Medium | Classify (net vs TLS), surface "fetch error: …", retry with exponential backoff (`fetch.c`, 20→…→300 s) |
| Publisher interrupted with lock held | Medium | Record the owner PID, preserve live locks, recover dead PID locks immediately; age-gate ownerless legacy locks (`codexbar-publish.sh`) |
| Upstash auth failure (401/403) | Low | Distinct "auth (token?)" status; never log the bearer token; refuses non-`https://` URLs before sending |
| Malformed / oversized payload | Medium | Parse + clamp at the boundary (`stats_model.c`: NaN/range guards, forward version gate); oversize → "response too big"; never crash, show "bad data from store" |
| OpenCode Go API unavailable or cookie expired | Medium | Retain the local 30-day history and publish `oc.fresh:false`; the publisher preserves the last genuinely fresh LKG timestamp, so cached values remain visible but still age out |
| WiFi association lost / AP flaps | Medium | Reconnect with escalating backoff; roam the ≤5 remembered networks; self-heal to add-network portal only after grace + zero known SSIDs |
| Corrupt NVS blob (`wnets`) | Low | Validate magic/size/version/count; treat invalid as "zero networks"; never erase or brick on a bad blob |
| Heap fragmentation (long uptime) | Low | Static/long-lived buffers for the fetch/parse path; avoid per-poll churn where practical |
| Unbounded `publish.log` growth | Medium | Rename and gzip-archive above `CBPUB_LOG_MAX_BYTES`, prune to `CBPUB_LOG_KEEP` (default 3), and repoint publisher stdio to a fresh live inode; `--rotate-log` forces it manually |
| Publisher curl transport failure under launchd (`HTTP 000`, empty body) | Medium | curl's exit code + `%{errormsg}` are captured and its `curl: (N) …` stderr line is kept in the log; the failure is classified (dns/connect/timeout/tls/local-curl-config) and logged as `publish transport FAILED (curl exit N, class …)` with **exit 6**; the store is left untouched (fail-soft) and the next cycle retries. `--check-transport` runs a read-only GET with the same curl/flags to locate the fault (publisher-local vs launchd-environment vs curl-stack vs http-auth) |

## Publisher exit codes

`codexbar-publish.sh` uses distinct exit codes so launchd's *last exit code* (and
`pipeline-check.py`) can name the failing stage without log archaeology:

| Code | Meaning | Store effect |
|------|---------|--------------|
| 0 | published (or nothing to do after a clean cycle) | updated / unchanged |
| 2 | usage error (unknown argument) | untouched |
| 3 | skip: no fresh data — keeps last good | untouched |
| 4 | producer failure (`codexbar-stats.sh` could not produce a payload) | untouched |
| 5 | config/token error (missing URL, missing/invalid token or `CURL_BIN`) | untouched |
| 6 | publish **transport** failure — no HTTP response (`http_code 000`) | untouched |
| 7 | publish **HTTP** failure — a non-2xx response (auth/rate-limit/server) | untouched |

### curl transport classes (`HTTP 000`)

`HTTP 000` is curl's `%{http_code}` for "no HTTP response at all" — it is never an
Upstash status. The publisher (and checker) map curl's exit code to a class:

| curl exit | class | likely cause |
|-----------|-------|--------------|
| 6 | `dns` | name resolution failed |
| 7 | `connect` | TCP connect refused/failed |
| 28 | `timeout` | connect or transfer exceeded `--connect-timeout 10` / `--max-time 30` (flaky network, captive portal) |
| 26 | `local-curl-config` | could not read the `-K` config or `--data-binary` file |
| 35/51/53/54/58/60/66/77/80/83/90/91 | `tls` | TLS handshake / certificate / CA-bundle problem |

A transient timeout cluster that self-recovers (the toy keeps showing the last
good payload and the next cycle republishes) is expected fail-soft behavior, not
a defect. Per-invocation `--retry` is deliberately **not** used: the `StartInterval`
cycle already provides recovery at cadence, and retries would only lengthen a
failing cycle without bridging a multi-minute outage.

## Invariants

These are guarantees the firmware must uphold regardless of input. Violations
are bugs, not edge cases.

1. **Never brick on bad runtime data.** Parsers (`stats_model.c`) and validators
   (`config_store.c`) handle any input without aborting. Malformed data is
   logged and rejected; the device shows an error state and keeps running.
2. **The bearer token never leaks.** It rides in the request header only — never
   in a log line, never in the response-body snippet, never on the display.
3. **Resource cleanup is guaranteed.** Every `esp_http_client_init` is paired
   with `cleanup`; every mutex take with a give. Leaked handles/locks are bugs.
4. **Shared state is mutex-guarded.** `st` is written by setters and the nav
   machine under `s_mtx` and read only by `ui_task`; `_locked` helpers assume
   the caller holds it.
5. **Schema changes fail closed.** An unknown version (`v` ∉ {1, 2}) is rejected,
   not best-effort rendered.

## Testing strategy

- **Host-runnable unit tests** under `firmware/test/<module>/` (one Makefile +
  `./runtests` per suite). Each suite exercises a single module against
  hand-written fixtures:
  - `firmware/test/stats_model/` — payload parser, uses
    `docs/references/*.sample.json` and deliberately-malformed fixtures
    (truncated bodies, mixed types, unknown `v`) to verify the parser's
    fail-closed behavior.
  - `firmware/test/config_store/` — WiFi LRU store, including blob
    corruption paths.
  - `firmware/test/ui_format/` — integer-cents / token formatting helpers.
- **On-device manual QA** — see `docs/testing-checklist.md` for the
  behavior-level checklist (provisioning, reconnect, page nav, long-press
  add-network). End-to-end flows against a real Upstash key + real WiFi are
  exercised by the user during install, not by automated CI.
