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
