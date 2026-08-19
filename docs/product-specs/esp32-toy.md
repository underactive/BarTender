# Spec: ESP32 desk toy

## User story

As someone who wants ambient awareness of my AI-plan usage, I want a small
always-on desk device that shows each provider's current usage at a glance,
without my laptop being involved, so I can see "how much room is left" without
opening anything.

## Acceptance criteria

- [ ] First boot with no Upstash **or** zero remembered WiFi → WPA2 SoftAP;
      the screen shows the SoftAP SSID + password and the URL to open.
- [ ] The **full** captive form (WiFi SSID/pass + Upstash URL + Redis key +
      read-only token) is served only when Upstash is not yet set. Once
      Upstash is saved the form is **WiFi-only** (SSID/pass); the URL/key/
      token are kept and never re-rendered into the HTML.
- [ ] Up to **5** WiFi networks are remembered (MRU-ordered). Adding a 6th
      evicts the least-recently-*used* one. Re-adding an existing SSID
      updates its password in place (no duplicate, no count growth).
- [ ] On boot / after a disconnect the device **scans** and autoconnects to
      the strongest remembered network in range (8 dB stickiness toward the
      last one). Moving between remembered locations needs no interaction.
- [ ] Changing/adding a WiFi network never requires re-entering Upstash.
- [ ] A device flashed over an older single-SSID build keeps working: the
      legacy SSID is folded into the remembered list, Upstash is preserved,
      no erase and no forced portal.
- [ ] After provisioning the device joins WiFi and, within ~15 s, shows one
      row per provider: id, a colored bar plus its display percentage for the
      primary window. Quota-backed providers show headroom (`100 - used%`);
      Pi/MiMo/LM Studio baseline ratios retain the used percentage. When Pi
      Agent local usage exists on the Mac, `pi` appears as a first-class row in
      this same provider list rather than a separate screen.
- [ ] Providers reported `ok:false` render as a dimmed "off", not blank.
- [ ] A status line shows link state and "updated Ns ago".
- [ ] The screen renders in portrait orientation (taller than wide); the
      provider-row layout adapts to the panel width so bars/percentages are
      not clipped on either orientation.
- [ ] On the **summary screen**: vertical swipe scrolls the provider list;
      tap a provider row opens its Cost page (tap again cycles Cost↔Limit;
      swipe right→left returns). Pi's Cost page uses the same chrome but shows
      today's Pi spend/tokens, 30-day maxes, and a 30-day spend graph. A **long-press
      (~1.5 s)** opens the captive portal to **add a network**,
      NON-destructively (all remembered networks + Upstash are kept). Full
      nav: [claude-cost-menu](claude-cost-menu.md).
- [ ] The long-press add-network gesture is honored **even before WiFi has
      ever associated** (e.g. relocated where no remembered SSID is in
      range), not only once connected. Nothing on-device wipes credentials.
- [ ] Rendered quota remaining percentages equal `100 - used%` from a `curl
      GET` of the same Upstash key; baseline-relative activity percentages
      preserve the source ratio.
- [ ] OpenRouter, MiMo, Moonshot, DeepSeek, and Ramp show a `$X.XX` prepaid-balance
      headline when a balance is published, with a segmented $10 bar: segment
      count is `ceil(balance/$10)` capped at 10, the final segment is partial,
      and missing balance falls back to the normal percentage tile.
- [ ] Balances over $100 render each completed $100 as a filled circle to the
      right of the bar; the bar shrinks to fit and shows the remaining $0-100
      window on a gauge divided into quarters (e.g. $150 -> half-full quartered
      bar + one circle; $300 -> full bar + two circles). Circle count is
      `ceil(balance/$100) - 1`, so exact multiples prefer a full bar. Balances
      at or below $100 keep the tenths ($10-per-segment) gauge.
- [ ] No secret is compiled into the firmware or committed to the repo.

## Edge cases

| Scenario | Expected behavior |
|----------|-------------------|
| WiFi drops (after connecting once this boot) | Rescans and autoconnects to any remembered network now in range (capped 5→15→60 s between sweeps); last data stays on screen. Never self-reprovisions and never wipes — a transient blip must not discard creds |
| Relocated where a *different* remembered network is present | Scans, finds it, autoconnects, promotes it to MRU — zero interaction |
| Relocated where NO remembered network is in range | "WiFi: no known network"; a long-press still opens the add-network portal, and after `CONNECT_GRACE_S` (180 s) AND ≥2 empty scan sweeps it auto-opens that portal **non-destructively** (keeps the ≤5 list + Upstash) |
| A remembered SSID's password was changed at that location | "WiFi: wrong password &lt;ssid&gt;"; that SSID is skipped for the sweep and others are tried; recover by re-adding it via the portal |
| Open / WPA2-Enterprise / web-login venue WiFi | Not associated (WPA2-PSK only, by design); device shows "no known network" rather than hanging — out of scope |
| Hidden SSID | Best-effort: one direct connect attempt to the MRU entry per empty sweep; a fully-cloaked AP may not autoconnect |
| Bad/expired token | "fetch error: auth (token?)" banner; retry every 20 s |
| Key absent / publisher not run yet | "waiting for publisher…" (reachable, no value) |
| Malformed JSON | "bad data from store" banner |
| Response larger than buffer | "response too big" (defensive; v2 payload ~0.6–2 KB, `body[4096]`) |
| Display image rotated | Documented bring-up flag flip (`BOARD_LCD_MIRROR_X/Y`) |

## Not in scope

- Pushing/writing anything back to Upstash (device is read-only).
- Open, WPA2-Enterprise, or web-login ("click to agree") venue WiFi — the
  device has no browser and enforces a WPA2-PSK threshold by design.
- On-screen text entry (provisioning is via the phone/laptop captive form).
- Sound, animation, battery operation.
- Historical graphs **on the summary screen** — but 30-day provider graphs now
  exist on Cost cards where reduced history is available (Claude/Codex spend,
  Pi Agent spend); see [claude-cost-menu](claude-cost-menu.md).
- NVS/flash encryption — now a **tracked** hardening item (risk elevated at
  payload v2; see `docs/SECURITY.md` and the tech-debt tracker).
