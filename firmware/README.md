# CodexBar Toy firmware (Prompt 3 of 3)

ESP-IDF firmware for the **Freenove ESP32-S3 2.8" (FNK0104)** — joins WiFi,
polls the Upstash key over HTTPS every 5 min, and renders the CodexBar usage
stats on the ILI9341 screen. The board bring-up (ILI9341 + LVGL 9, FT6336
touch, shared I2C, NVS) is **vendored from the proven `clawd-tank` project**;
only WiFi / HTTPS / JSON / the stats UI / captive-portal provisioning are new.

## Prerequisites

- ESP-IDF **5.3+** (`idf.py` on PATH, e.g. via the project's `.envrc`/direnv)
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

## First-boot provisioning (captive portal)

With no credentials in NVS the device starts a WPA2 SoftAP and shows the
join info **on its own screen**:

1. Screen shows `WiFi: CodexBar-Toy-XXXX` + a password.
2. Join that network from a phone/laptop. The captive sheet opens
   automatically (or browse `http://192.168.4.1/`).
3. Enter: home WiFi SSID/password, Upstash REST URL, Redis key
   (default `codexbar`), and the **read-only** Upstash token.
4. Submit → values saved to NVS → device reboots and runs.

Nothing secret is ever compiled into the binary or committed to the repo.

## Normal operation

- Polls `GET {url}/get/{key}` every `FETCH_INTERVAL_S` (300 s) over TLS
  (Mozilla CA bundle — no cert pinning).
- Renders one row per provider: id, a colored bar + % for the primary
  window; `off` (dimmed) when that provider is `ok:false`. A status line
  shows link state and "updated Ns ago".
- **Tap** the screen → immediate refresh.
- **Triple-tap within 2 s** → wipe credentials and reboot into the
  captive portal (re-provision; the FNK0104 has no BOOT button).

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
    config_store.*                         ← NVS (pattern reused, new keys)
    app_event.h net_wifi.* upstash.* stats_model.* ui.* fetch.* main.c  ← new
```
