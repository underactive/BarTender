# Reliability

Requirements and practices for keeping the system reliable.

## Failure modes to handle

| Failure | Likelihood | Mitigation |
|---------|------------|------------|
| Upstash unreachable / TLS / DNS error | Medium | Classify (net vs TLS), surface "fetch error: …", retry with exponential backoff (`fetch.c`, 20→…→300 s) |
| Upstash auth failure (401/403) | Low | Distinct "auth (token?)" status; never log the bearer token; refuses non-`https://` URLs before sending |
| Malformed / oversized payload | Medium | Parse + clamp at the boundary (`stats_model.c`: NaN/range guards, forward version gate); oversize → "response too big"; never crash, show "bad data from store" |
| WiFi association lost / AP flaps | Medium | Reconnect with escalating backoff; roam the ≤5 remembered networks; self-heal to add-network portal only after grace + zero known SSIDs |
| Corrupt NVS blob (`wnets`) | Low | Validate magic/size/version/count; treat invalid as "zero networks"; never erase or brick on a bad blob |
| Heap fragmentation (long uptime) | Low | Static/long-lived buffers for the fetch/parse path; avoid per-poll churn where practical |

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

- **Unit tests:** Core logic, parsers, and validators are unit-tested with
  fixture data. One fixture per input format or edge case.
- **Integration tests:** End-to-end flows against a real (or realistic)
  environment. These are slower and may run only in CI.
- **Chaos fixtures:** Deliberately malformed input (truncated data, mixed
  formats, unexpected types) to verify resilience at system boundaries.
