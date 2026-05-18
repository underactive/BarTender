# Spec: ESP32 desk toy

## User story

As someone who wants ambient awareness of my AI-plan usage, I want a small
always-on desk device that shows each provider's current usage at a glance,
without my laptop being involved, so I can see "how much room is left" without
opening anything.

## Acceptance criteria

- [ ] First boot with no creds → WPA2 SoftAP; the screen shows the SoftAP
      SSID + password and the URL to open.
- [ ] The captive form accepts WiFi SSID/pass + Upstash URL + Redis key +
      read-only token; on submit they persist to NVS and the device reboots.
- [ ] After provisioning the device joins WiFi and, within ~15 s, shows one
      row per provider: id, a colored bar + % for the primary window.
- [ ] Providers reported `ok:false` render as a dimmed "off", not blank.
- [ ] A status line shows link state and "updated Ns ago".
- [ ] Tap → immediate refresh. Triple-tap within 2 s → wipe creds + reboot
      to the captive portal.
- [ ] Rendered percentages match a `curl GET` of the same Upstash key.
- [ ] No secret is compiled into the firmware or committed to the repo.

## Edge cases

| Scenario | Expected behavior |
|----------|-------------------|
| WiFi drops | "reconnecting…" + capped backoff (5→15→60 s); last data stays on screen |
| Bad/expired token | "fetch error: auth (token?)" banner; retry every 20 s |
| Key absent / publisher not run yet | "waiting for publisher…" (reachable, no value) |
| Malformed JSON | "bad data from store" banner |
| Response larger than buffer | "response too big" (defensive; payload is ~450 B) |
| Display image rotated | Documented bring-up flag flip (`BOARD_LCD_MIRROR_X/Y`) |

## Not in scope

- Pushing/writing anything back to Upstash (device is read-only).
- On-screen text entry (provisioning is via the phone/laptop captive form).
- Historical graphs, sound, animation, battery operation.
- NVS/flash encryption (documented hardening follow-up).
