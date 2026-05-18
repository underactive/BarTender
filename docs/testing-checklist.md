# Testing Checklist

Observable, behavior-level checks. Hardware items are user-run on the
Freenove ESP32-S3 (FNK0104); host items are off-device where a seam exists.

## Firmware — audit-fix behavior changes (2026-05-18)

- [ ] **Over-large setup form rejected, not silently corrupted.** Submit a
      provisioning form whose body exceeds ~1.5 KB → the page says "Form too
      large or empty"; the device does NOT reboot and NOT persist creds.
- [ ] **Normal provisioning still works** (regression). Valid form → "Saved.
      Rebooting…" → device connects and renders stats.
- [ ] **Corrupt store value surfaces as an error.** Set the Upstash key to a
      non-string (e.g. a number) → device shows "bad data from store", not
      "waiting for publisher…".
- [ ] **Unknown schema version is refused.** Publish a payload with `v` ≠ 1
      → device shows "bad data from store" (forward guard).
- [ ] **Captive portal still auto-opens** on iOS and Android after the DNS
      responder hardening (no regression in the "sign in to network" sheet).
- [ ] **Freshness counter is honest.** Right after a fetch the status line
      reads "… updated 0s ago" and increments roughly every 10 s; it never
      freezes for ~10 s after new data arrives.
- [ ] **Reconnect backoff escalates** 5 → 15 → 60 s when the AP flaps
      (previously could stay at the first delay).
- [ ] **Re-provision gesture is deliberate.** 3 fast taps (within ~1.2 s)
      still wipe creds + reboot to the portal; slow or occasional taps only
      force a refresh and never reset.
- [ ] **No secrets on the wire/log.** UART log during provisioning shows the
      SoftAP SSID but NOT its password; the Upstash token never appears in
      any log line.

## Host (deferred — recipe, not yet built)

- [ ] `stats_model_parse` table tests (valid; `result` null → NO_DATA;
      `result` non-string → BAD; bad inner JSON → BAD; `v`≠1 → BAD;
      >12 providers capped; `ok:false` minimal entry; float `p`).
- [ ] `provision.c` `urldecode`/`field` tests (`+`→space, `%XX`, malformed
      `%`, prefix-collision `a` vs `ab`, missing field, over-length clamp).

Seam: a `firmware/test/host/` CMake target compiling `stats_model.c` +
`provision.c` parser parts against host cJSON with a no-op `esp_log.h` shim.
