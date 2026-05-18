# Plan: ESP32-S3 CodexBar desk-toy firmware (Prompt 3 of 3)

- **Started:** 2026-05-18
- **Status:** Completed 2026-05-18 — built clean (1.37 MB, exit 0, only a
  vendored-touch.c deprecation warning), flashed to the FNK0104, first boot
  reached the captive portal, and after user provisioning the device boots
  STA → HTTPS GET → parse → renders live usage bars/% on the ILI9341.
  Full 3-stage pipeline verified end-to-end on hardware.
- **Objective** — ESP-IDF firmware for the Freenove ESP32-S3 2.8" (FNK0104):
  WiFi + HTTPS GET the Upstash key + render CodexBar stats on the ILI9341;
  captive-portal provisioning; read-only token.

## Context

Completes the 3-stage roadmap. The user's `clawd-tank` project already runs
ESP-IDF 5.3 + LVGL 9.5 on this exact board, so the board layer is vendored
verbatim and only WiFi/HTTPS/JSON/UI/provisioning is new. Build/flash is the
user's step (`idf.py` not available in the dev session); confidence comes
from (a) vendoring proven modules unchanged and (b) building the JSON parser
against the real 446-byte Upstash envelope captured live.

## Changes

- **Vendored verbatim** (clawd-tank, byte-identical): `firmware/main/`
  `board_config.h`, `i2c_bus.[ch]`, `display.[ch]`; `firmware/partitions.csv`.
- **Vendored + retargeted**: `touch.[ch]` (posts `APP_EVT_TOUCH` instead of a
  BLE event).
- **New**: `app_event.h`, `config_store.[ch]` (NVS, new keys),
  `net_wifi.[ch]`, `provision.[ch]` (SoftAP + HTTP form + captive DNS),
  `upstash.[ch]` (HTTPS + esp_crt_bundle), `stats_model.[ch]` (two-step
  cJSON), `ui.[ch]` (single-task LVGL screen), `fetch.[ch]`, `main.c`.
- **Build**: `CMakeLists.txt`, `main/CMakeLists.txt`, `main/idf_component.yml`,
  `main/Kconfig.projbuild`, `sdkconfig.defaults`, `sdkconfig.defaults.esp32s3`.
- **Docs/conventions**: ARCHITECTURE.md populated, `.gitignore` ESP-IDF set,
  README, EXTERNAL_INTEGRATIONS, SECURITY, QUALITY_SCORE, product spec,
  memory, this plan + PLANS index.
- **clawd-tank: not modified.**

## Dependencies

Prompt 2's Upstash key + a read-only token (user-created). LVGL/esp_lcd
managed components auto-fetched on first `idf.py build`.

## Decisions

- 2026-05-18: Standalone `codexbarjar/firmware/` vendoring the board layer
  (vs. editing clawd-tank or a shared component) — keeps repos decoupled.
- 2026-05-18: Captive-portal → NVS provisioning (no secrets in source).
- 2026-05-18: Consolidated UI into one `ui.c` with a copy-state-under-mutex
  pattern — all LVGL calls confined to the UI task (deviation from the plan's
  separate `stats_screen.c`; simpler + thread-safe; reset-hint per row
  dropped for 320x240 legibility — follow-up).
- 2026-05-18: `esp_crt_bundle` over cert pinning.

## Steps

- [x] Capture live Upstash bytes; design parser to them
- [x] Vendor board layer (sha-verified identical)
- [x] Scaffold ESP-IDF project + build files
- [x] New modules (wifi/provision/upstash/model/ui/fetch/main)
- [x] Docs & conventions
- [ ] **User**: `idf.py set-target esp32s3 && idf.py build && … flash` →
      acceptance checklist (below)
- [ ] Post-implementation audit (docs/AUDIT.md) → move plan to completed/

## Verification

Done here: parser shaped against the real 446 B envelope (float `p`,
`ok:false` carries only id+ok, missing-`result` → NO_DATA); board layer
sha-identical to clawd-tank; new modules reviewed (clang diagnostics are
ESP-IDF-headers-absent noise, not defects; real misses — `<stdbool.h>`,
`<stdint.h>` — were caught and fixed).

User acceptance checklist: first boot → on-screen SoftAP SSID/pass → submit
form → reboot → WiFi connects → provider rows with live %s; tap → refresh;
drop WiFi → "reconnecting" + backoff; bad token → "auth" banner; triple-tap
→ portal; rendered %s match `curl GET` of the key.

## Risks / open questions

- No on-device compile here — residual risk in new WiFi/HTTP/LVGL code;
  mitigated by reuse + parse-against-real-bytes + acceptance checklist.
- ILI9341 orientation/mirror flags are a clawd-tank bring-up TODO; if the
  image is rotated, flip `BOARD_LCD_MIRROR_X/Y` together (board_config.h).
- NVS creds unencrypted (physical-access only) — flash encryption follow-up.
- Captive auto-popup varies by OS; `http://192.168.4.1/` is the fallback.

---

# Post-implementation audit (2026-05-18)

Per `docs/AUDIT.md`: 3 parallel reviewers (QA+Resource/Concurrency;
Security+Interface-Contract; State+DX+Testing) over the **new** modules
(vendored board layer excluded — sha-identical to clawd-tank, runs on
hardware). Report written here (the plan was already in `completed/`).

## Files changed by audit fixes

| File | Findings addressed |
|------|--------------------|
| `firmware/main/provision.c` | DNS validation (CRITICAL), body-truncation/timeout (HIGH), PSK-in-log (LOW) |
| `firmware/main/upstash.c` | snprintf-truncation (HIGH), cross-host redirect (HIGH), error classification (MED) |
| `firmware/main/stats_model.c` | result-type mapping (MED), schema-version guard (MED) |
| `firmware/main/ui.c` | age-freshness gap + shared-state hack (HIGH), mutex-skip render (HIGH) |
| `firmware/main/net_wifi.c` | backoff-decoupled-on-flap (LOW), lock-free-flag doc (LOW) |
| `firmware/main/fetch.c` | accidental triple-tap factory-reset (MED, mitigated) |

## Findings & disposition

- `[FIXED]` **CRITICAL (provision.c dns_task)** — unvalidated DNS responder
  (UDP reflection vector; malformed replies). Now drops responses (QR set),
  requires QDCOUNT==1, walks QNAME, answers A only, truncates trailing/EDNS.
- `[FIXED]` **HIGH (provision.c h_save)** — over-long/partial POST body
  silently truncated → corrupted secrets persisted while replying "Saved".
  Now reads exactly `Content-Length`, 413 on too-large/empty, 500 on hard
  socket error, retries on `HTTPD_SOCK_ERR_TIMEOUT`; buffer 640→1536.
- `[FIXED]` **HIGH (upstash.c)** — unchecked `snprintf` truncation of URL/
  Authorization (a clipped URL could resolve to a different CA-valid host and
  replay the bearer token). Now checks both return values; buffers derived
  from `CFG_*_MAX`.
- `[FIXED]` **HIGH (upstash.c)** — `disable_auto_redirect=false` forwarded the
  bearer token across redirects. Now `true` (Upstash `/get` never 30x's).
- `[FIXED]` **HIGH (ui.c)** — "updated Ns ago" froze ~10 s after a fetch
  (`last_age_ms` not reset) and the render path mutated shared `st.status`
  (save/restore-under-mutex hack, magic `48`). `render()` now composes the
  age from `st.fetched_ms` into a local buffer; `ui_task` re-dirties every
  10 s; the hack is gone.
- `[FIXED]` **HIGH (ui.c)** — `xSemaphoreTake(s_mtx, 0)` skipped the render
  entirely under setter contention. Now blocks 5 ms.
- `[FIXED]` **MEDIUM (stats_model.c)** — present-but-non-string `result`
  mapped to NO_DATA ("waiting for publisher"). Split: absent/null → NO_DATA,
  non-string → BAD ("bad data from store").
- `[FIXED]` **MEDIUM (stats_model.c)** — unknown schema `v` rendered as
  best-effort. Now `v != 1` → BAD (forward guard).
- `[FIXED]` **MEDIUM (upstash.c)** — non-TLS perform failures mislabeled
  "tls". Now classified by the `ESP_ERR_ESP_TLS_BASE` range.
- `[FIXED]` **MEDIUM (fetch.c)** — accidental triple-tap could factory-reset
  over weeks of uptime (no spacing constraint). Tightened: 3 taps within
  1200 ms AND each gap ≤ 600 ms (touch.c debounces 200 ms). *Mitigated, not
  eliminated — see deferred.*
- `[FIXED]` **LOW (net_wifi.c)** — `esp_timer_start_once` on an armed timer
  was ignored while backoff index advanced (decoupled schedule on a flapping
  AP). Now `esp_timer_stop()` first.
- `[FIXED]` **LOW (provision.c)** — SoftAP PSK printed to the UART log.
  Removed (still shown on the device screen for the user).
- `[FIXED]` **LOW (net_wifi.c)** — undocumented lock-free `volatile` flag;
  added a `// WHY:` so it isn't "fixed" with a needless mutex later.

### Deferred (accepted, with rationale)

- **Reprovision confirmation UI (MED, fetch.c)** — an on-screen "tap again to
  confirm reset" would fully eliminate accidental resets. Deferred: the
  tightened temporal gating makes a stray trigger highly unlikely, and a
  confirmation flow is UX scope beyond the POC. The gesture is documented.
- **Early-wake on WiFi reconnect (LOW, fetch/net_wifi)** — up to 20 s of
  stale "reconnecting…". Deferred: acceptable on a 5-min toy; needs a
  net_wifi→fetch signal. Documented.
- **Sticky AUTH backoff (LOW, upstash/fetch)** — a bad token retries forever
  at the normal interval. Deferred: harmless at 5-min cadence; the fix is to
  re-provision (documented), and infinite identical retry self-heals if the
  token is corrected upstream.
- **`config_store` over-long value → "unprovisioned" (LOW)** — accepted:
  `CFG_TOKEN_MAX` (256) exceeds real Upstash tokens (~80), and the truncation
  paths that could produce over-long values were fixed above.
- **Host test harness (Testing)** — no off-hardware tests. Deferred with a
  concrete recipe (below). Building a second build system is out of scope for
  this audit pass; the highest-value pure logic (`stats_model_parse`,
  `provision.c` `urldecode`/`field`) is identified for a future `firmware/
  test/host/` target against host cJSON + a 10-line `esp_log.h` shim.
- **`field()`/`urldecode` robustness (MED/LOW)** — correct for the fixed form
  field order + browser encoding; documented limitation, locked down later by
  the host-test recipe.

## Audit Fixes

**Fixes applied** (each references the finding above):
1. provision.c DNS query validation — Security/QA CRITICAL.
2. provision.c Content-Length-bounded body read + 413/500 — Security HIGH.
3. upstash.c snprintf truncation checks + CFG_*-derived buffers — Security HIGH.
4. upstash.c `disable_auto_redirect=true` — Security HIGH.
5. upstash.c esp-tls-range error classification — Contract MED.
6. stats_model.c absent/null vs non-string `result` split — Contract MED.
7. stats_model.c `v != 1` forward guard — Contract MED.
8. ui.c render()-composes-age + removed shared-state hack — State HIGH.
9. ui.c mutex take with 5 ms timeout — State/QA HIGH.
10. net_wifi.c `esp_timer_stop()` before re-arm — QA LOW.
11. fetch.c tightened triple-tap window — QA MED (mitigated).
12. provision.c removed PSK from log — Security LOW.
13. net_wifi.c lock-free-flag `// WHY:` — DX LOW.

**Verification checklist** (post-fix; build is clean, exit 0):
- [ ] Submit a provisioning form whose total body > 1536 B → "Form too large"
      (413), nothing written to NVS.
- [ ] Submit a normal form → saves, reboots, connects (regression).
- [ ] Point the key at a non-string value (`SET codexbar 123`) → device shows
      "bad data from store", not "waiting for publisher".
- [ ] Publish with `v` bumped to 2 → device shows "bad data from store".
- [ ] Captive portal still pops on iOS/Android (DNS hardening regression).
- [ ] After a fresh fetch the "updated Ns ago" counter starts at ~0 and ticks
      every ~10 s (no freeze).
- [ ] Kill WiFi mid-run → reconnect backoff still escalates 5→15→60 s.
- [ ] 3 deliberate fast taps still re-provision; slow/occasional taps do not.
- [ ] `idf.py build` clean (verified 2026-05-18, 1.36 MB, exit 0).

Behavior-changing fixes are tracked in `docs/testing-checklist.md`.
