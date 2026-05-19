# Plan: Persistent Upstash + Multi-WiFi (LRU, scan/autoconnect)

- **Started:** 2026-05-18
- **Completed:** 2026-05-18
- **Status:** Completed (build-clean + 4-reviewer audit; on-device pending user flash)
- **Objective** — Decouple Upstash creds from WiFi creds and remember up to 5
  WiFi networks (LRU-evicted) so the toy scans and autoconnects when moved
  between locations without re-entering Upstash.
- **Changes** — config_store.{h,c} (5-entry MRU NVS blob + has_upstash +
  set_upstash + one-shot portal flag + legacy migration); net_wifi.{h,c}
  (manager task: scan-on-disconnect → select → connect → MRU-promote;
  event handler reduced to a pure signaller); provision.{h,c} + ui.{h,c}
  (WiFi-only captive form + ASCII status strings); fetch.{c,h} (non-destructive
  `enter_portal()` + self-heal gated on "no known network"); main.c (split
  has_upstash / wifi_count / forced-portal boot branch); docs (spec, SECURITY,
  ARCHITECTURE, QUALITY_SCORE, READMEs, tech-debt).
- **Dependencies** — config_store API lands first (everything depends on it);
  net_wifi + provision/ui depend on it; fetch + main wire them together;
  build clean before docs/audit; audit + plan-completion last.
- **Risks / open questions** — see "Risks & mitigations" in the design plan
  (`~/.claude/plans/tingly-jingling-brook.md`): self-heal data-loss (removed),
  portal boot-loop (clear-before-act), scan tearing the link (scan only while
  disconnected), lock-free invariant (event task stays a pure signaller), blob
  corruption (validate + fail to zero networks), auth-loop (per-sweep skip),
  open/captive venues (out of scope by user decision).

## Context

The firmware stores one WiFi SSID/pass and treats WiFi + Upstash as one
inseparable "provisioned" blob; `config_store_clear_provisioning()` wipes all
five keys, so changing WiFi forces re-entering the Upstash URL + read-only
token and the device can hold only one network. Goal: persist Upstash across
WiFi changes, remember ≤5 networks (least-recently-*used* evicted), and
scan+autoconnect to whichever remembered network is in range so relocating
between home / work / café "just works".

Locked decisions (confirmed with user): triple-tap & 180 s self-heal are
**non-destructive** (open an add-network portal, keep all creds); **WPA2-PSK
only** (open/captive venues out of scope); captive form is **WiFi-only** when
Upstash already set (token never re-rendered); selection = strongest in-range
RSSI + 8 dB stickiness, scan only while disconnected; "oldest" = LRU
(MRU-promote on every successful association).

## Steps
- [x] config_store.{h,c}: blob schema + validation + new API + legacy migration
- [x] net_wifi.{h,c}: manager task (scan/select/connect/promote) + signaller events
- [x] provision.{h,c} + ui.{h,c}: WiFi-only form + ASCII status strings
- [x] fetch.{c,h} + main.c: non-destructive enter_portal() + split boot branch
- [x] `idf.py -C firmware build` clean (1.50 MB, 82% free)
- [x] Doc updates (AGENTS.md steps 7–12)
- [x] Post-impl: Files-changed/Summary/Verification/Follow-ups filled;
      4-reviewer audit run + fixes applied; moved to completed/; PLANS.md updated
- [ ] User on-device verification (6-scenario + audit-fix checklists) — needs flash

## Decisions
- 2026-05-18: Single NVS blob (not indexed keys) — atomic LRU rotate in one
  commit, matches existing single-commit discipline.
- 2026-05-18: Upstash stays separate string keys — must survive WiFi changes
  independently; a corrupt WiFi blob must never endanger the token.
- 2026-05-18: net_wifi-owned manager task (not fetch-driven) — keeps the WiFi
  domain self-contained; event task stays the sole `s_connected` writer so the
  documented lock-free invariant (net_wifi.c:12-15) is preserved.
- 2026-05-18: Persisted one-shot `fprov` flag with clear-before-act — boot-loop
  safe and fits the existing reboot-to-portal pattern (no RAM restructuring).
- 2026-05-18: Keep global WPA2_PSK threshold per user decision — legacy open
  network already fails today, so no regression.

## Open questions
- (resolved during planning via AskUserQuestion — none outstanding)

## Files changed

Code (firmware/main/):
- `config_store.h` / `config_store.c` — `wifi_creds_t` blob schema + CFG_WIFI_*;
  wifi_count/get/add_or_update/promote/clear_all; has_upstash; set_upstash;
  request_portal/take_portal_request (clear-before-act); blob load+validate;
  idempotent legacy migration; legacy fns kept as no-ops; dropped dead
  `nvs_flash.h`.
- `net_wifi.h` / `net_wifi.c` — full rewrite: `net_wifi_start_multi`,
  `net_wifi_no_known_network`; `wifi_mgr` task (scan-on-disconnect → RSSI/
  stickiness select → connect → MRU-promote); pure-signaller `on_wifi`;
  per-sweep auth skip-set; named constants.
- `provision.h` / `provision.c` — `provision_start(bool)`; `FORM_WIFI`
  WiFi-only form; `h_save` split (no secret echo); `s_wifi_only`.
- `ui.h` / `ui.c` — `st.prov_wifi_only`; 3-arg `ui_set_provisioning`;
  branched provisioning copy.
- `fetch.h` / `fetch.c` — `enter_portal()` (non-destructive, re-entrancy
  guard) replaces `reprovision()`; self-heal gated on
  `net_wifi_no_known_network()` && `CONNECT_GRACE_S`; updated docs.
- `main.c` — split has_upstash / wifi_count / one-shot portal boot branch;
  `provision_start(have_upstash)`; `net_wifi_start_multi()`.
- `touch.c` — stale triple-tap comment corrected (doc only).

Docs: `docs/product-specs/esp32-toy.md`, `docs/SECURITY.md`,
`ARCHITECTURE.md` (decision #10 + domain row), `docs/QUALITY_SCORE.md`,
`firmware/README.md`, `README.md`, `docs/exec-plans/tech-debt-tracker.md`,
`docs/PLANS.md`.

## Implementation summary

Built as planned. Upstash (url/rkey/tok string keys) is fully decoupled from
WiFi (one validated `cbtoy/wnets` MRU blob, ≤5, atomic LRU). A net_wifi-owned
`wifi_mgr` task scans only while disconnected, picks the strongest in-range
remembered SSID (8 dB stickiness, MRU tie-break), connects, and MRU-promotes
on GOT_IP; the event handler stays a pure signaller (lock-free `s_connected`
+ `s_last_disc_reason` confirmed race-free by audit). Triple-tap and the
180 s self-heal are non-destructive (one-shot `fprov`, clear-before-act,
re-entrancy-guarded) and the self-heal only fires when ≥2 sweeps saw zero
remembered SSIDs. Captive form is WiFi-only when Upstash is set (token never
rendered). Legacy single-SSID devices migrate with no erase / no forced
portal. Deviation from plan: no generic ASCII-escape helper added to `ui.c`
— `net_wifi.c` sanitizes its own SSID status strings (`san()`), keeping the
modules decoupled with less churn; provisioning shows only device-generated
ASCII so the helper was unnecessary. `idf.py build` clean (1.50 MB, 82% free).

## Audit (4 parallel reviewers covering all 7 AUDIT.md personas)

`[FIXED]` items were resolved this session; unmarked items are accepted/deferred.

- `[FIXED]` **R&C§P1-1 / QA§P0** `await_outcome` stale binary-latched
  notification → premature give-up / stale disconnect-reason. Fix:
  `xTaskNotifyStateClear(NULL)` before each `esp_wifi_connect()` so only the
  in-flight attempt's GOT_IP/DISCONNECTED can wake it. (net_wifi.c)
- `[FIXED]` **R&C§P1-3** `s_mgr` published after `esp_wifi_start()` → dropped
  STA_START wake; `volatile` caused a qualifier-discard warning. Fix:
  `xTaskCreate` (publishes `s_mgr`) moved BEFORE `esp_wifi_start()`; reverted
  volatile (task creation is a full barrier). (net_wifi.c)
- `[FIXED]` **QA§P1-2** `esp_wifi_scan_get_ap_records` return ignored → stale
  `s_recs`/`n` could drive a connect. Fix: bail the sweep on non-OK. Also
  `s_sweeps_no_cand` now resets whenever a remembered SSID was *seen* (not
  only on association), so an unjoinable/transient AP can't spuriously trip
  the self-heal portal; new "cannot join saved network" status. (net_wifi.c)
- `[FIXED]` **QA§P2-3** re-entrant `enter_portal` during the 900 ms window —
  added a static idempotency guard. (fetch.c)
- `[FIXED]` **DX§P2-4** dead `nvs_flash.h` include removed. (config_store.c)
- `[FIXED]` **Interface§P2-8 / DX** stale `touch.c` comment + misleading
  "unused legacy" grouping clarified (migration uses internal `get_str`, not
  the public wrappers). (touch.c, config_store.h)
- `[FIXED]` **DX§P1/P2** named magic numbers (`SWEEPS_NO_KNOWN_MIN`,
  `SWEEPS_SAT`), backoff rationale, blob magic-bump caveat. (net_wifi.c,
  config_store.h)
- *Accepted/deferred* — **QA§P1-3** 32-octet SSID truncates to 31 via
  `strlcpy` into `wc.sta.ssid[32]`: documented in-code + follow-up; rare,
  reworking string storage out of scope. **Interface§P1** ignored
  `blob_save`/`promote` returns: already `ESP_LOGI(...FAIL)` inside
  config_store; no extra handling added (fail-safe: prior valid blob
  persists). **Testing**: no host harness — pre-existing tracked debt;
  `config_store` LRU + `select_candidate` flagged as prime host-test targets
  (tech-debt-tracker updated). Lock-free `s_connected`/`s_last_disc_reason`
  invariant **confirmed race-free** by the security/concurrency reviewer.

### Audit Fixes — verification checklist
- [ ] Two saved nets, move A↔B: connects to whichever is present, promotes it.
- [ ] Wrong saved password at an in-range net: shows "wrong password",
      skips it, does NOT open the self-heal portal (saw_candidate gate).
- [ ] Fresh boot, no known net in range ≥2 sweeps + 180 s: add-network
      portal opens; the ≤5 list + Upstash survive.
- [ ] Rapid triple-tap burst: `enter_portal` runs once (guard), reboots once.
- [ ] Add 6th network: LRU evicted; re-add existing SSID: in-place, count flat.
- [ ] Flash over old single-SSID build (no erase): migrates, no portal.
- [ ] Triple-tap → WiFi-only form: `curl http://192.168.4.1/` shows no
      url/token inputs and no token string anywhere.

## Verification

`idf.py -C firmware build` clean, 1.50 MB (82% app partition free), zero
warnings/errors across all 8 recompiled `main/*.c` (host-clangd `-mlongcalls`
/ `../hal.h` noise is not a real toolchain result). No host test harness
exists (tracked debt) → on-device verification is the 6-scenario script in
the design plan + the Audit Fixes checklist above, handed to the user (the
device is not flashed by this session). Lock-free concurrency invariant
audited and confirmed; LRU rotate/evict math audited and confirmed correct
in all 6 cases.

## Follow-ups

- On-device run of the 6-scenario + audit-fix checklists (user; needs flash).
- 32-octet SSID limitation (practical limit 31) — only matters for a
  maximal-length SSID; revisit only if hit in the field.
- Host-test seam (`firmware/test/host/`) for `config_store` LRU/migration and
  `net_wifi` `select_candidate` — logged in tech-debt-tracker.
- NVS encryption — pre-existing tracked debt; scope widened (≤5 PSKs at rest).
