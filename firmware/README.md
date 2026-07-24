# CodexBar Toy firmware (Prompt 3 of 3)

ESP-IDF firmware for the **Freenove ESP32-S3 2.8" (FNK0104)** — joins WiFi,
polls the Upstash key over HTTPS every 60 s (FETCH_INTERVAL_S), and renders the CodexBar usage
stats on the ILI9341 screen. The board bring-up (ILI9341 + LVGL 9, FT6336
touch, shared I2C, NVS) is **vendored from the proven `clawd-tank` project**;
only WiFi / HTTPS / JSON / the stats UI / captive-portal provisioning are new.

## Prerequisites

- ESP-IDF **5.3+** (`idf.py` on PATH, e.g. via `source $IDF_PATH/export.sh`)
- The FNK0104 board over USB-C
- An Upstash Redis DB with a **read-only** token (created in Prompt 2)

## Build & flash

```sh
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/tty.usbmodem* flash monitor
```

Managed deps (LVGL 9, esp_lcd_ili9341, esp_lcd_touch_ft5x06) are fetched
automatically on first build.

## Provisioning (captive portal)

If **Upstash is not set OR no WiFi network is remembered**, the device
starts a WPA2 SoftAP and shows the join info **on its own screen**:

1. Screen shows `WiFi: CodexBar-Toy-XXXX` + a password.
2. Join that network from a phone/laptop. The captive sheet opens
   automatically (or browse `http://192.168.4.1/`).
3. **Full form** (Upstash not yet set): home WiFi SSID/password, Upstash
   REST URL, Redis key (default `codexbar`), and the **read-only** token.
   **WiFi-only form** (Upstash already set — e.g. you long-pressed to add
   a network): just the new WiFi SSID/password. You never re-enter Upstash,
   and the token is never sent back to the browser.
4. Submit → saved to NVS → device reboots and runs.

Nothing secret is ever compiled into the binary or committed to the repo.

## WiFi: remembered networks & roaming

- The device remembers **up to 5** WiFi networks (most-recently-used
  order). Adding a 6th evicts the least-recently-*used*. Re-adding an
  existing SSID just updates its password in place.
- On boot and after any disconnect it **scans** and autoconnects to the
  strongest remembered network in range (8 dB stickiness toward the last
  one). Moving between home / work / a saved café needs no interaction.
- Changing or adding WiFi never requires re-entering Upstash.
- **WPA2-PSK only.** Open, WPA2-Enterprise, and web-login ("click to
  agree") venue WiFi are unsupported by design (no browser on the device).
- Flashing over an older single-SSID build keeps working: the old SSID is
  folded into the remembered list, Upstash preserved, no erase.

## Normal operation

- Polls `GET {url}/get/{key}` every `FETCH_INTERVAL_S` (60 s) over TLS,
  validating the full hostname/certificate chain against the embedded ISRG Root
  X2 public CA anchor (no leaf-certificate pinning or verification bypass).
- Renders one row per provider: id, a colored bar + % for the primary
  window; `off` (dimmed) when that provider is `ok:false`. A status line
  shows link state, "updated Ns ago", and ` +N more` when the list is
  longer than the screen.
- **Swipe up / down** → scroll the provider list (page at a time).
- **Tap a provider row** → its **Cost** page. **Tap again** → **Usage
  Limits** page. Tap again → Cost (tapping cycles the two). Cost pages use
  an autoscaled bar graph; Non-Claude Cost pages show "COST DATA NOT
  AVAILABLE YET".
- **Swipe right→left** → back to the summary list.
- Refresh is automatic (the 60 s poll); there is no tap-to-refresh.
- **Long-press (~1.5 s) on the summary** → open the captive portal to
  **add a WiFi network** (the FNK0104 has no BOOT button). **Non-
  destructive:** all remembered networks **and** Upstash are kept. Honored
  **even before WiFi has ever associated**, so a relocated device can still
  be set up by hand.
- **Self-heal:** if a fresh boot finds **no remembered network in range**
  for `CONNECT_GRACE_S` (180 s, confirmed by ≥2 empty scan sweeps) it opens
  that same add-network portal on its own — also non-destructive. Once
  associated, a later disconnect just rescans/roams; nothing is ever wiped.

## Security

The device holds only the **read-only** Upstash token — it can read the
already-non-sensitive usage JSON but cannot modify the store. Credentials
live in NVS (unencrypted; physical-access threat only — flash encryption is
a documented hardening follow-up). See `../docs/SECURITY.md`.

## Layout

```
firmware/
  CMakeLists.txt  sdkconfig.defaults[.esp32s3]  partitions.csv
  main/
    board_config.h i2c_bus.* display.*   ← vendored verbatim from clawd-tank
    touch.*                                ← vendored; event retargeted
    config_store.* wifi_lru.h              ← NVS (pattern reused, new keys)
    app_event.h net_wifi.* upstash.* stats_model.* fetch.* main.c  ← new
    ui.* ui_format.c ui_render_core.c ui_render_card.c ui_render_summary.c ui_screensaver.c ui_internal.h     ← UI (split by concern)
    led.* boot_splash.* provision.* screenshot.*                    ← new
    provider_icons.* provider_colors.h font_*.c                     ← generated/assets
```
