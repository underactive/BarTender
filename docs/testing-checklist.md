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
- [ ] **Unknown schema version is refused.** Publish a payload with
      `v` ∉ {1, 2} (e.g. `v: 3`) → device shows "bad data from store"
      (forward guard; firmware accepts v1 and v2).
- [ ] **Captive portal still auto-opens** on iOS and Android after the DNS
      responder hardening (no regression in the "sign in to network" sheet).
- [ ] **Freshness counter is honest.** Right after a fetch the status line
      reads "… updated 0s ago" and increments roughly every 10 s; it never
      freezes for ~10 s after new data arrives.
- [ ] **Reconnect backoff escalates** 5 → 15 → 60 s when the AP flaps
      (previously could stay at the first delay).
- [ ] **Re-provision gesture is deliberate.** A ~1.5 s long-press on the
      summary opens the add-network portal (non-destructive — keeps the ≤5
      networks + Upstash); a quick tap opens a provider page and never
      re-provisions; nothing on-device wipes credentials.
- [ ] **No secrets on the wire/log.** UART log during provisioning shows the
      SoftAP SSID but NOT its password; the Upstash token never appears in
      any log line.
- [ ] **TLS failures are explicit and fail closed.** A rejected HTTPS
      certificate shows `fetch error: tls`, never `network`, and the client
      does not send an authenticated request after the failed handshake.
- [ ] **Current Upstash TLS chain fetches.** The live `*.upstash.io` endpoint
      accepts its ISRG/Let's Encrypt chain and renders the stored payload.

## Firmware — scrollable summary + tap-cycle pages (2026-05-18, nav redesign)

Gesture regression (highest risk — input FSM + recovery gesture):

- [ ] **Summary vertical swipe scrolls** the provider list a page at a time;
      over-scroll at either end is a no-op; the ` +N more` status hint shows
      the count hidden BELOW the current window (0 at the bottom).
- [ ] **Tap a provider row → its Cost page**; **tap again → Usage Limits**;
      tap again → Cost (cycles). The tap maps to the row under the finger
      using the live scroll offset.
- [ ] **Swipe right→left returns to the summary** with scroll preserved;
      left→right is inert; swipe-left on the summary itself is inert.
- [ ] **Long-press (~1.5 s) on the summary** opens the add-network portal
      (non-destructive). A quick tap never opens it; 3 fast taps just cycle
      pages (the triple-tap detector is gone).
- [ ] **Exactly one gesture per press**: a ~200 ms tap never long-presses;
      a hold that then moves >40 px swipes (no long-press); the 12–40 px
      drag dead-zone is still inert.
- [ ] **Long-press is never lost** even if spammed during a blocking fetch
      (the queue is flushed + the long-press re-sent on a full queue).

Rendering:

- [ ] **Summary** shows the windowed provider rows for the current scroll
      offset; tapping a row opens that provider's page (no menu screen
      exists anymore).
- [ ] **Summary token total counts to refreshed values** over the same short
      ease-out animation used by the card hero metrics, without restarting
      on each summary redraw.
- [ ] **I/O TOKENS respects the local-day boundary.** Shortly after Pacific
      midnight, the hero is near zero and then grows through the day; the first
      morning Codex session never causes a stale-yesterday total to cliff down.
- [ ] **Claude Cost card**: `$X.XX` today (no `f%` artifact), `<n>M TOKENS
      TODAY`, `30D $… • …`, a **labeled 30-day spend bar graph** with side
      margins, autoscaled so the highest point fills the chart, `EXTRA $a / $b`
      bar.
- [ ] **Claude Usage-Limits card**: session remaining % == `100 - p`;
      weekly remaining % == `100 - s`; reset hints == `pr`/`sr`; plus a **24h
      SESSION remaining-% line sparkline** with side margins.
- [ ] **Codex/Cursor/OpenRouter Cost card** shows "COST DATA NOT AVAILABLE
      YET"; their Usage-Limits card still shows real remaining session/weekly
      % and **hides** the 24h sparkline (no `ph` for them).
- [ ] **Quota bars and percentages read as "headroom" (inverted default).** A
      low quota usage % renders a high remaining percentage and a NEARLY-FULL
      bar; a high quota usage % renders a low remaining percentage and a
      NEARLY-EMPTY bar — on the summary rows AND both cards. Pi, MiMo, and LM
      Studio baseline-relative activity ratios retain their used percentage and
      overage magnitude. Bar COLOR still follows true usage (green low → red
      high), independent of fill. "off"/no-data bars stay empty. (`ui_set_bar_invert(false)`
      / flip `UI_BAR_INVERT_DEFAULT` restores the classic used-fill direction
      for quota bars.)
- [ ] **Over-100% usage bars pulse.** When a provider's true session % exceeds
      100 (e.g. Pi or Cursor on a record day), its summary tile bar and
      Usage-Limits session bar animate: saturated accents (Cursor teal, etc.)
      fade opacity in the accent color; light accents (Pi) cycle grey↔white;
      bars at or below 100% stay static.
- [ ] **Prepaid balance tiles**: OpenRouter, MiMo, Moonshot, DeepSeek, and Ramp show
      `$X.XX` plus a segmented $10 bar when a balance is available: `$0` is one
      empty segment, `$18` is one full + 80% of a second, `$40` is four full,
      and `$92` is nine full + 20% of a tenth (10 segments total). Other
      providers retain their percentage label and solid bar. Reuse a tile slot
      between balance and percentage providers without stale dividers/ranges.
- [ ] **Balances over $100 add filled circles**: each completed $100 draws as a
      filled accent circle to the left of the bar, which shrinks and shifts
      right to fit so the circles start where a plain bar would. The bar then
      shows the remaining $0-100 window with no dividers at all (not tenths):
      `$150` -> one circle + half-full plain bar (`o==--`), `$300` -> two
      circles + full bar (`oo====`), `$611.95` -> six circles + nearly-empty
      plain bar. Balances `<= $100` keep the tenths gauge. Reusing the
      slot for a `<= $100` balance or a percentage tile leaves no leftover
      circles.
- [ ] **Summary rows show the provider's CodexBar logo** in the left margin,
      tinted with that provider's accent (light grey if un-themed), spanning
      the name + bar lines (two-line row). A provider with no bundled icon
      shows text only (no broken/empty box). Icons regenerate via
      `python3 scripts/build/gen-provider-icons.py` from the vendored SVGs.
- [ ] **No tofu glyphs** anywhere (ASCII-only); layout not clipped in the
      live 240×320 orientation.
- [ ] **End-to-end**: `curl GET {url}/get/{key}` → device Cost numbers +
      chart shape match the payload's `cost` block.

## Host (deferred — recipe, not yet built)

- [ ] `stats_model_parse` table tests (valid v1 & v2; `result` null →
      NO_DATA; `result` non-string → BAD; bad inner JSON → BAD; `v`∉{1,2} →
      BAD; v2 `cost` block parsed (ct/cm/tt/tm/xu/xl/h); v1 → `has_cost`
      false; `hist` capped at `STATS_HIST_MAX`; >12 providers capped;
      `ok:false` minimal entry; float `p`).
- [ ] Publisher cost-merge JXA vs a mock cost-cache dir: rollup math, churn
      fallback (no `days` → usage-only, exit 3), `files` map never read.
- [ ] `provision.c` `urldecode`/`field` tests (`+`→space, `%XX`, malformed
- [ ] `provision.c` `urldecode`/`field` tests (`+`→space, `%XX`, malformed
      `%`, prefix-collision `a` vs `ab`, missing field, over-length clamp).

Seam: a `firmware/test/host/` CMake target compiling `stats_model.c` +
`provision.c` parser parts against host cJSON with a no-op `esp_log.h` shim.
