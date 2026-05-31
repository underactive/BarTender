# Plan: Fowler-audit refactoring backlog

- **Started:** 2026-05-31
- **Completed:** 2026-05-31
- **Status:** Completed — all 9 items done & verified (idf.py build clean +
  host suites green). UI extractions build-verified; stats_model/ui_format
  changes additionally covered by 130 + 69 host assertions. #8 (scripts)
  verified at module + syntax level only (live publish pipeline not exercised).
- **Objective** — Work through the 9 deferred findings from the Martin Fowler
  persona audit (see commit `395fd18` for the 6 already-applied items). All
  are behavior-preserving structural refactors. Highest-priority items first;
  each is marked complete ONLY after its verification gate passes.
- **Verification gate** — `idf.py build` clean (the firmware compiles `-Werror`)
  for every UI-layer change, plus `make run` green in the affected
  `firmware/test/*` host suite where one exists. The `stats_model` change
  (#7) is additionally covered by 130 host assertions; the new render-helper
  tests (#3) become the safety net for the `ui_render.c` extractions.
- **Done when** — every item below is checked `[x]`.

## Context

`ui_render.c` (~1680 lines) is the audit's center of gravity: 7 of the 9
findings live there. It exhibits file-level Divergent Change — it grows for
unrelated reasons (summary layout, cost cards, limit tiers, grid overlay).
The two high-severity Long Methods (`build_widgets`, `render_limits_card`)
are the structural payoff; Fowler flagged that they sit in the one layer with
no host-test coverage, so #3 (render-helper tests) is the implicit enabler.

ESP-IDF is installed at `~/esp/esp-idf`; `. ~/esp/esp-idf/export.sh` then
`idf.py build` from `firmware/` compiles the UI files. Baseline confirmed
green on 2026-05-31 before this work started.

## Items (priority order)

### High
- [x] **#1 — `ui_render.c:102` `build_widgets` Long Method (~370 lines).**
  Extract `build_summary_widgets()`, `build_cost_card()`, `build_limits_card()`,
  `build_grid_overlay()` called from `build_widgets()`. Verify: `idf.py build`.
  ✅ Done 2026-05-31 — build clean (-Werror); binary 0x2456a0 vs 0x2456b0
  baseline (−16 B codegen). Creation/paint order preserved exactly.
- [x] **#2 — `ui_render.c:1181` `render_limits_card` Long Method (~185 lines).**
  Extract per-tier `render_limits_auto/_weekly/_extra/_sparkline`.
  Verify: `idf.py build`. ✅ Done 2026-05-31 — build clean (-Werror); binary
  0x2456a0 (byte-identical). Preserved weekly-before-extra ordering
  (extra's OpenRouter branch repurposes lim.w_bar).

### Medium
- [x] **#3 — `ui_render.c` render logic has no host tests (safety net gap).**
  Add a `firmware/test/ui_format/` host suite covering the pure helpers
  (`provider_kind`, `bar_fill`, `extra_pct`, `pct_tenths`, `fmt_*`,
  `summary_provider_name`, `clampi`). Verify: `make run` green.
  ✅ Done 2026-05-31 — new suite (lvgl/freertos shim), **69 assertions pass**.
  Covers clampi/bar_fill/pct_tenths/extra_pct/provider_kind/summary_provider_name/
  fmt_money/fmt_pct/fmt_tokens/fmt_tokens_full/up_id/is_hidden_provider/
  provider_has_limits_card/i64_hist_to_i32/provider_tok_today/pct_color/bar_color.
- [x] **#4 — `ui_render.c:1255` repeated show/hide tier triples.**
  Added `show_tier_row()` / `hide_tier_row(...,rst)` helpers (NULL-able reset
  label) instead of a struct, since lim.x_* (no reset) isn't uniform with
  lim.a_*/w_*. Verify: `idf.py build`. ✅ Done 2026-05-31 — build clean;
  binary 0x245690 (−16 B). Replaced 7 clear/add-flag blocks.
- [x] **#5 — `ui_render.c:1629` summary-row Long Method.**
  Extract `render_summary_row()` / `render_summary_secondary_bar()`.
  Verify: `idf.py build`. ✅ Done 2026-05-31 — build clean; binary 0x2455e0.
  Off-screen hide path left inline (distinct concern).

### Low
- [x] **#6 — `ui_render.c:858` `render_lmstudio_chart`/`render_cursor_chart`
  duplication.** Extract a parameterized `render_token_chart_card(...)`.
  Verify: `idf.py build`. ✅ Done 2026-05-31 — build clean; binary 0x245510
  (−smaller, dedup). Both originals now thin wrappers; show_requests flag.
- [x] **#7 — `stats_model.h:28` tier data clump (Primitive Obsession).**
  `has_p/p/pr × 3` → `usage_tier_t {has; pct; reset}` as named members
  primary/secondary/tertiary (not `tier[3]` — avoids magic indices across 42
  render sites). 76 references migrated across 7 files. Verify: `make run`
  (130 + 69 host assertions) + `idf.py build`. ✅ Done 2026-05-31 — all green;
  binary 0x245520. Compiler-net migration (renamed, not aliased); host tests
  confirm parse/format tier mapping.
- [x] **#8 — `scripts/cursor-stats.sh:414` duplicated history load/save/prune.**
  Extracted `scripts/_stats_history.py` (load/save/prune). Scoped to
  cursor + lmstudio only — pi uses a different event-reducer mechanism, not the
  rolling-dict pattern (per user decision). Heredoc-piped Python imports it via
  an exported `CBTOY_SCRIPT_DIR` + `sys.path` inject; scripts run in-place
  (`${0:A:h}`), so the sibling module always resolves. ✅ Done 2026-05-31 —
  module `py_compile` OK + load/save/prune/corrupt round-trip asserted; both
  scripts' embedded Python parses. NOTE: the live publish pipeline (Cursor
  cookie / running LM Studio) was NOT exercised here — verification is
  module-level + syntax, not end-to-end. lmstudio's history write is now atomic
  (was a direct write) — a latent robustness gain, output unchanged.

### Info
- [x] **#9 — `ui_render.c:804` `place_hero_amount` parallel functions.**
  Added `hero_style_t` font/offset descriptor + shared `place_hero_styled()`;
  the two named functions are now thin wrappers (call sites unchanged).
  Verify: `idf.py build`. ✅ Done 2026-05-31 — build clean; binary 0x245520
  (unchanged; descriptors + wrappers inline away).

## Log

- 2026-05-31 — Doc created; baseline `idf.py build` green.
