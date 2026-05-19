# Spec: Swipe-navigated provider menu with Claude Cost & Usage-Limits cards

## User story

As the owner of the CodexBar toy, I want to swipe down on the summary screen to
open a menu of my providers, drill into Claude, and see its real cost (today,
30-day, tokens, 30-day history) and usage limits (session %, weekly %,
extra-usage $) on dedicated cards, so the desk toy is a glanceable spend +
limits monitor, not just a usage-% bar list.

## Acceptance criteria

- [ ] **Swipe down on the summary opens the menu.** Row 0 = "SUMMARY"
      (hardcoded); the rest = one row per provider that has stats on the
      summary screen (`ok` providers), uppercased, with ≥44 px tap targets.
- [ ] **Tap a provider → submenu** "COST" / "USAGE LIMITS". Tap one → its card.
- [ ] **Swipe up goes back exactly one level** (card → submenu → menu →
      summary). Tap selects the row under the finger. Swipe down on the
      summary is the only way in.
- [ ] **Legacy gestures are summary-only.** A tap on the summary still forces a
      refresh; a deliberate triple-tap still factory-resets. Inside the
      menu/cards, taps select and NEVER refresh or factory-reset.
- [ ] **Claude Cost card** shows: header `CLAUDE COST`; today `$` big number;
      `<tokens> TOKENS TODAY`; `30D $<m> • <tokens>`; a labeled 30-day
      daily-spend **line sparkline** ("N-DAY SPEND • max $X", side margins,
      point_count == real history length); and an `EXTRA <used> / <limit>`
      bar (extra-usage overage).
- [ ] **Claude Usage-Limits card** shows: `SESSION` + big % + bar + reset;
      `WEEKLY <%>` + bar + reset; `EXTRA USAGE <used> / <limit>` + bar; and a
      labeled **24h SESSION usage-% line sparkline** ("SESSION 24H • now N%",
      from payload `ph`, Claude only — hidden when absent).
- [ ] **Bars show headroom (inverted default).** Every progress bar (summary
      rows + both cards) fills 0% → full, 100% → empty. Bar color still tracks
      true usage % (green low → red high). Switchable via
      `UI_BAR_INVERT_DEFAULT` / `ui_set_bar_invert()` (future portal setting).
- [ ] **Provider color theme.** Each provider's bars + charts use that
      provider's **CodexBar brand color** (mirrored from CodexBar's
      `WidgetColors.color(for:)` — e.g. Claude `0xCC7C5E`, Codex `0x49A3B0`,
      Cursor `0x00BFA5`, OpenRouter `0x6F42C1`). Unknown providers fall back
      to the green/amber/red usage ramp. Bar color is independent of fill.
- [ ] **Provider logo on summary rows.** Each summary row shows the
      provider's CodexBar logo (A8 silhouette, accent-tinted) in the left
      margin, spanning the two-line row. Source: `scripts/assets/codexbar-logos/`
      (vendored from CodexBar) → `scripts/gen-provider-icons.py` →
      `firmware/main/provider_icons.c`. Providers without a bundled icon
      render text-only.
- [ ] **Cost numbers match CodexBar.** Today/30-day `$` and tokens equal the
      rollup of `~/Library/Caches/CodexBar/cost-usage/claude-v*.json` `days`
      aggregates; verifiable via `curl GET` of the Upstash key.
- [ ] **Non-Claude Cost cards show a placeholder** ("COST DATA NOT AVAILABLE
      YET"); their Usage-Limits cards still render real session/weekly % from
      the existing `p`/`s` fields.
- [ ] **Money/tokens render correctly** (no `f%` artifact): `$12.47`,
      `123.2M TOKENS`. All text is ASCII (no tofu glyphs).

## Edge cases

| Scenario | Expected behavior |
|----------|-------------------|
| CodexBar cost cache absent / schema churned | Publisher publishes usage-only; Cost card shows "COST DATA NOT AVAILABLE YET" |
| Provider drilled into disappears on refresh | Nav falls back to the menu level (no stale/garbage card) |
| Ambiguous drag (12–40 px) | Dead-zone: no event (neither tap nor swipe) |
| Horizontal swipe | Ignored (only vertical swipes navigate) |
| FT6336G drops a sample mid-swipe | 40 ms release grace absorbs it (no false tap/release) |
| Menu has more providers than rows that fit | Displayed rows capped to what fits; remainder not shown (documented) |
| v1 payload on a v2 device | Parses fine; `has_cost=false` → Cost card placeholder |

## Not in scope

- 24h *hourly* **cost** sparkline — CodexBar's cost cache is day-granular; the
  Cost-card "24h" view is the TODAY big number + the real 30-day daily chart
  (deviation from the original mock, see exec-plan `claude-cost-menu`
  Decision #3). NOTE: a 24h *usage-%* sparkline IS now shipped on the
  Usage-Limits card (`ph`, hourly data from a different CodexBar file).
- Per-model (Opus/Sonnet/Haiku) `$` breakdown — data exists in the cache but
  is intentionally deferred to a follow-up.
- Cost/history for Codex/Cursor/OpenRouter (placeholder this build).
- Scrolling a menu longer than the screen.
- Writing anything back to Upstash (device stays read-only).
