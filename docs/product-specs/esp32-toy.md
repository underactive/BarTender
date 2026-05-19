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
- [ ] The screen renders in portrait orientation (taller than wide); the
      provider-row layout adapts to the panel width so bars/percentages are
      not clipped on either orientation.
- [ ] On the **summary screen**: tap → immediate refresh; triple-tap within
      2 s → wipe creds + reboot to the captive portal. (Inside the swipe menu
      these gestures select/navigate instead — see
      [claude-cost-menu](claude-cost-menu.md).)
- [ ] Swipe down on the summary opens the provider menu; swipe up backs out
      one level. Full behavior: [claude-cost-menu](claude-cost-menu.md).
- [ ] Rendered percentages match a `curl GET` of the same Upstash key.
- [ ] No secret is compiled into the firmware or committed to the repo.

## Edge cases

| Scenario | Expected behavior |
|----------|-------------------|
| WiFi drops | "reconnecting…" + capped backoff (5→15→60 s); last data stays on screen |
| Bad/expired token | "fetch error: auth (token?)" banner; retry every 20 s |
| Key absent / publisher not run yet | "waiting for publisher…" (reachable, no value) |
| Malformed JSON | "bad data from store" banner |
| Response larger than buffer | "response too big" (defensive; v2 payload ~0.6–2 KB, `body[4096]`) |
| Display image rotated | Documented bring-up flag flip (`BOARD_LCD_MIRROR_X/Y`) |

## Not in scope

- Pushing/writing anything back to Upstash (device is read-only).
- On-screen text entry (provisioning is via the phone/laptop captive form).
- Sound, animation, battery operation.
- Historical graphs **on the summary screen** — but a 30-day Claude cost
  history chart now exists on the Cost card; see
  [claude-cost-menu](claude-cost-menu.md).
- NVS/flash encryption — now a **tracked** hardening item (risk elevated at
  payload v2; see `docs/SECURITY.md` and the tech-debt tracker).
