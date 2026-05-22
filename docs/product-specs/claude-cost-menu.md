# Spec: Scrollable summary + tap-cycle Claude Cost & Usage-Limits pages

## User story

As the owner of the CodexBar toy, I want to scroll a provider list that is
longer than the screen, tap a provider to see its real cost or local Pi Agent
rollups (today/30-day spend where available, Pi max spend/tokens, history) and
usage limits (session %, weekly %, extra-usage $) on dedicated pages, and tap
again to flip between Cost and Limit — so the desk toy is a glanceable spend +
limits monitor, not just a clipped usage-% list.

## Acceptance criteria

- [ ] **The summary scrolls.** Vertical swipe pages the provider list
      (swipe up → later providers, swipe down → earlier), clamped at both
      ends. A ` +N more` ASCII hint on the status line shows when the list is
      longer than the screen. There is no swipe-down menu anymore.
- [ ] **Tap a provider row → its Cost page.** The tap hit-tests the row
      under the finger using the live scroll offset (≥ ~40 px row targets).
- [ ] **Tap again cycles Cost ↔ Limit.** Tap on a page toggles between the
      Cost page and the Usage-Limits page for that provider (Cost first).
- [ ] **Swipe right→left returns to the summary** (scroll position
      preserved). Swipe-left on the summary itself is inert (it is the root).
- [ ] **Long-press (~1.5 s) on the summary** opens the captive portal to
      ADD a WiFi network — non-destructive (keeps the ≤5 networks + Upstash;
      see [esp32-toy](esp32-toy.md)). Honored even before WiFi associates.
      A quick tap opens a page; it never opens the portal.
- [ ] **Gestures off the summary never reach fetch.** Only a summary
      long-press returns `UI_INPUT_PASS`; taps/swipes/page interactions are
      all consumed by the nav machine.
- [ ] **Claude/Codex Cost page** shows: provider header; today `$` big number;
      `<tokens> TOKENS TODAY`; `30D $<m> • <tokens>`; and a labeled 30-day
      daily-spend **line sparkline** ("N-DAY SPEND • max $X", side margins,
      point_count == real history length). Providers with extra-usage data
      also expose that budget on the Usage-Limits page.
- [ ] **Pi Cost page** shows: header `PI STATS`; max daily spend as the big
      `$` number; max daily tokens with a `max tokens` label; `30 DAY MAX:
      $<max> • <tokens>`; and a Pi-colored 30-day daily-spend line sparkline
      from the reduced `pi.h` payload field. It reuses the existing provider
      page chrome and does not add a Pi-only navigation path.
- [ ] **Claude Usage-Limits page** shows: `SESSION` + big % + bar + reset;
      `WEEKLY <%>` + bar + reset; `EXTRA USAGE <used> / <limit>` + bar; and a
      labeled **24h SESSION usage-% line sparkline** ("SESSION 24H • now N%",
      from payload `ph`, Claude only — hidden when absent).
- [ ] **Bars show headroom (inverted default).** Every progress bar (summary
      rows + both pages) fills 0% → full, 100% → empty. Bar color still tracks
      true usage % (green low → red high). Switchable via
      `UI_BAR_INVERT_DEFAULT` / `ui_set_bar_invert()` (future portal setting).
- [ ] **Provider color theme.** Each provider's bars + charts use that
      provider's **CodexBar brand color** (e.g. Claude `0xCC7C5E`, Codex
      `0x49A3B0`, Cursor `0x00BFA5`, OpenRouter `0x6F42C1`). Unknown providers
      fall back to the green/amber/red usage ramp. Color is independent of fill.
- [ ] **Provider logo on summary rows.** Each summary row shows the
      provider's CodexBar logo (A8 silhouette, accent-tinted) in the left
      margin, spanning the two-line row. Source: `scripts/assets/codexbar-logos/`
      → `scripts/gen-provider-icons.py` → `firmware/main/provider_icons.c`.
      Providers without a bundled icon render text-only.
- [ ] **Cost numbers match local sources.** Claude/Codex today/30-day `$` and
      tokens equal the rollup of CodexBar cost-cache `days` aggregates;
      Pi max spend/tokens and history equal the reduced rollup of Pi Agent
      `~/.pi/agent/sessions/**/*.jsonl` usage rows. Both are verifiable via
      `curl GET` of the Upstash key.
- [ ] **Every provider gets both pages.** Providers without reduced cost/Pi
      data show a placeholder ("COST DATA NOT AVAILABLE YET"); their Limit
      pages render real session/weekly % from `p`/`s`. Tapping still cycles
      both pages. Pi opens Cost first when its `pi` block is present.
- [ ] **Money/tokens render correctly** (no `f%` artifact): `$12.47`,
      `123.2M TOKENS`. All text is ASCII (no tofu glyphs), including the
      ` +N more` scroll hint.

## Edge cases

| Scenario | Expected behavior |
|----------|-------------------|
| Provider list longer than the screen | Scrolls; ` +N more` hint; page-step per swipe; clamped at both ends (over-scroll = no-op) |
| Provider drilled into disappears on refresh | Nav falls back to the summary (no stale/garbage page); scroll re-clamped if the list shrank |
| Provider on a page goes `ok:false` on refresh | Stays on the page — Limit still renders, Cost shows its placeholder (not ejected on a transient blip) |
| CodexBar cost cache absent / schema churned | Publisher publishes usage-only for that provider; Cost page shows "COST DATA NOT AVAILABLE YET" |
| Pi Agent sessions absent / malformed | Publisher omits the Pi provider and keeps publishing the rest of the payload |
| Ambiguous drag (12–40 px) | Dead-zone: no event (neither tap nor swipe nor long-press) |
| Left→right horizontal swipe | Ignored (only right→left = "back") |
| Hold then move > 40 px before 1.5 s | Swipe wins; no long-press (one gesture per press) |
| FT6336G drops a sample mid-swipe | 40 ms release grace absorbs it (no false tap/release) |
| v1 payload on a v2 device | Parses fine; `has_cost=false` → Cost-page placeholder |

## Not in scope

- 24h *hourly* **cost** sparkline — CodexBar's cost cache is day-granular; the
  Cost-page "24h" view is the TODAY big number + the real 30-day daily chart
  (deviation from the original mock, see exec-plan `claude-cost-menu`
  Decision #3). A 24h *usage-%* sparkline IS shipped on the Limit page.
- Per-model (Opus/Sonnet/Haiku) `$` breakdown — data exists in the cache but
  is intentionally deferred to a follow-up.
- Cost/history for Cursor/OpenRouter (placeholder unless their live payload exposes balance/cost fields).
- Manual refresh gesture — refresh is purely the 300 s poll now that tap is
  reassigned to navigation (long-press on a *page* is reserved/free for a
  future manual refresh).
- Writing anything back to Upstash (device stays read-only).
