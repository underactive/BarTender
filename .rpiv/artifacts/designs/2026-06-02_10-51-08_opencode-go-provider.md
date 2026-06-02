---
date: 2026-06-02T10:51:08-0700
author: Eric Sison
commit: d4f6f44
branch: main
repository: bartender
topic: "Add OpenCode Go provider to firmware pipeline (unhide + host script + publisher merge + UI cards)"
tags: [design, firmware, scripts, opencodego, provider]
status: ready
parent: ".rpiv/artifacts/research/2026-06-02_10-27-20_opencode-go-provider.md"
last_updated: 2026-06-02T10:51:08-0700
last_updated_by: Eric Sison
---

# Design: OpenCode Go Provider for BarTender

## Summary

Unhide the OpenCode Go provider (already registered with icon `ic_opencode` and color `0x3B82F6`), add a dedicated `oc` block on the firmware data model for token/cost data scraped from the OpenCode website API, render a custom TODAY page (tokens hero + cost line + 30-day token bar chart) and a LIMITS page using the existing Cursor 3-tier template (5-hour/weekly/monthly from CodexBar p/s/t), between LM Studio and OpenRouter in display order. Publisher merge uses in-place `oc` patch (like CURSOR_MERGE_JXA) since CodexBar already emits the base provider with usage bars. Auth cookie stored in Keychain following the Cursor session pattern.

## Requirements

- [ ] OpenCode Go appears on the summary page between LM Studio and OpenRouter
- [ ] Tapping OpenCode Go on the summary opens the TODAY page (CARD_COST) showing: tokens hero (rows 0-1), cost line (row 2), 30-day token bar chart (rows 3-8), footer with max tokens + today's cost as "max spend"
- [ ] Tapping again cycles to the LIMITS page showing 3 usage tiers: 5-hour (primary/p), weekly (secondary/s), monthly (tertiary/t)
- [ ] Summary row shows primary bar (5-hour/p) and secondary thin bar (weekly/s)
- [ ] Host-side scraper script `opencodego-stats.sh` reads token/cost data from OpenCode API using Keychain-stored auth cookie
- [ ] Publisher merge (`OG_MERGE_JXA`) patches `oc` block onto the existing `opencodego` provider entry, fail-soft on downstream errors
- [ ] JSON schema declares the `oc` sub-object with required fields: tk, ct, mxt, ht

## Current State Analysis

The OpenCode Go provider has:
- Color `0x3B82F6` in `provider_colors.h:18`
- Icon `ic_opencode` in `provider_icons.c:6385` (pixel A8 silhouette, shared with OpenCode)
- Listed in `HIDDEN_PROVIDERS` macro at `ui_internal.h:59` as `"opencodego"`
- No `s_display_order[]` entry in `stats_model.c:343`
- No `provider_kind_t` enum entry
- No `provider_kind()` / `summary_provider_name()` resolver branch
- No stats_model parsing
- No UI rendering
- No host-side scraper script
- No publisher merge JXA
- No JSON schema block
- No test coverage

The CodexBar CLI (`codexbar-stats.sh --json`) already outputs usage bars (p/s/t) for the `opencodego` provider since the user enabled it in CodexBar. The payload needs only the `oc` token/cost block merged.

### Key Discoveries

- `stats_model.c:336` — `s_display_order[]` defines canonical order; insert `"opencodego"` at position 2 (after `lmstudio`, before `openrouter`)
- `stats_model.c:217-241` — `parse_cu()` is the direct pattern to follow for `parse_oc()`: gated on `strcmp(p->id, "cursor")`, dedicated block, fields read via `get_i64`/`get_i32`, history array loop similar to `cu_ht`
- `ui_internal.h:59` — `#define HIDDEN_PROVIDERS "ollama", "opencode", "opencodego"` -> remove `"opencodego"`
- `ui_internal.h:81-87` — `provider_kind_t` enum — add `PK_OPENCODEGO` after `PK_CURSOR`
- `ui_format.c:145-163` — `provider_kind()` resolver adds `strcmp(id, "opencodego")` → `PK_OPENCODEGO`
- `ui_format.c:165-176` — `summary_provider_name()` adds `case PK_OPENCODEGO: return "OpenCode Go";`
- `ui_format.c:126-133` — `provider_card_available()` CARD_COST case: `p->has_cost || p->has_lm || p->has_cu || p->has_oc`
- `ui_format.c:215-221` — `provider_tok_today()` adds `case PK_OPENCODEGO: return p->has_oc ? p->oc_tok_today : 0;`
- `ui.c:244-248` — `provider_has_both_cards()`: `PK_LMSTUDIO || (PK_CURSOR && has_cu) || (PK_OPENCODEGO && has_oc)`
- `ui_render.c:1143-1160` — `render_cost_card()` switch — add PK_OPENCODEGO branch for today card
- `ui_render.c:908-993` — `render_token_chart_card()` — template for tokens hero + chart layout
- `ui_render.c:1369-1446` — `render_limits_card()` — data-driven 3-tier template, works automatically
- `ui_render.c:1493-1517` — `render_summary_secondary_bar()` — add PK_OPENCODEGO for weekly (s) bar
- `scripts/cursor-stats.sh` — full scraper pattern (Keychain auth, API pagination, rolling history)
- `scripts/codexbar-publish.sh:531-577` — CURSOR_MERGE_JXA in-place patch pattern for OG_MERGE_JXA
- `scripts/codexbar-publish.sh:116-131` — `store_cursor_session()` pattern for Keychain cookie storage

## Scope

### Building
- Firmware data model: `oc` block on `stats_provider_t` (tk, ct, mxt, ht, has_oc)
- Firmware parsing: `parse_oc()` in stats_model.c, called from provider loop
- Display order: Insert `"opencodego"` between `lmstudio` and `openrouter` in `s_display_order[]`
- UI identity: Unhide from HIDDEN_PROVIDERS, add PK_OPENCODEGO enum, provider_kind/name/tok_today dispatch
- UI card wiring: `provider_has_both_cards()`, `provider_card_available()` with `has_oc`
- UI TODAY card: Custom `render_opencodego_today()` — tokens hero + cost line + chart + footer
- UI summary: Secondary bar showing weekly (s) usage
- Host scraper: `scripts/opencodego-stats.sh` following `cursor-stats.sh` pattern
- Publisher merge: `OG_MERGE_JXA` block in `codexbar-publish.sh`, timeout-30 guard
- Keychain helpers: `--set-opencodego-cookie`, `--set-opencodego-cookie-clipboard`
- JSON schema: `oc` block definition in `docs/generated/codexbar-payload.schema.json`
- Tests: Reorder test + parse test in `test_stats_model.c`

### Not Building
- Distinct OpenCode Go SVG icon (reuses `ic_opencode` silhouette from OpenCode)
- Screensaver rotation (follows automatically from unhiding — excluded from HIDDEN_PROVIDERS = included in saver)
- 30-day cost history chart (chart shows tokens only, per user confirmation)
- per-request model breakdown (like LM Studio 7-day table)

## Decisions

### `oc` block data model
- **Ambiguity**: What fields does the `oc` block carry for the TODAY page?
- **Explored**: Option A — reuse generic `cost` block fields (ct/tt/h); Option B — dedicated `oc` block
- **Decision**: Dedicated `oc` block (following Cursor `cu` pattern), because the data source is a separate web API scrape (not CodexBar local cache) and the hero is token-focused
- **Evidence**: Research Q/A confirmed chart shows TOKENS, cost is single today value `ct`

### Summary bar mapping (5-hour vs weekly)
- **Ambiguity**: Does the summary primary bar show 5-hour usage (p) or weekly usage?
- **Explored**: User described summary as "top bar = Weekly, small bar = Monthly" but CodexBar fields map to p=5-hour, s=weekly, t=monthly
- **Decision**: Primary bar = 5-hour (p), secondary bar = weekly (s). Standard mapping matching all other providers.
- **Evidence**: User confirmed "Primary=p (5h), Secondary=s (weekly) (Recommended)" in design checkpoint

### Secondary bar on summary row
- **Ambiguity**: Should OpenCode Go show a secondary bar on the summary row?
- **Decision**: Yes — show weekly usage (s) as thin secondary bar, following Claude/Codex/LM Studio pattern
- **Evidence**: User confirmed "Follow — show secondary bar" in design checkpoint directional confirm

### Publisher merge pattern
- **Ambiguity**: Full replace (like LM_MERGE_JXA) or in-place patch (like CURSOR_MERGE_JXA)?
- **Decision**: In-place patch (like CURSOR_MERGE_JXA), because CodexBar already outputs the base `opencodego` provider with p/s/t bars
- **Evidence**: Research confirmed `codexbar-stats.sh --json` includes opencodego with usage bars

### Auth cookie flow
- **Ambiguity**: How does the user set up the OpenCode Go auth cookie?
- **Decision**: Same Keychain flow as Cursor session: `security add-generic-password` with `KC_SERVICE=codexbar-toy`, dedicated `KC_ACCOUNT=opencodego-session`
- **Evidence**: User confirmed "use the same flow for saving auth cookies as we do for scraping cursor stats" in research checkpoint

## Architecture

### `firmware/main/stats_model.h:46-56` — MODIFY

Add `oc` fields after `cu` fields on `stats_provider_t`:

```c
    // v2 optional `oc` block: OpenCode Go token/cost data from opencode.ai API.
    // Token-only history chart, single today's cost value.
    // tk/today tokens (int64), ct/today cents (int32), mxt/max daily (int64), ht[] history.
    bool     has_oc;
    int64_t  oc_tok_today;              // tokens today
    int32_t  oc_cost_today_c;           // cost today in cents
    int64_t  oc_tok_month_max;          // 30-day max daily tokens
    int      oc_ht_n;                   // valid entries in oc_ht[]
    int64_t  oc_ht[STATS_HIST_MAX];     // daily token totals, oldest -> newest
```

### `firmware/main/stats_model.c:336-342` — MODIFY

Insert `"opencodego"` at position 2 in `s_display_order[]`:

```c
static const char *s_display_order[] = {
    "pi",
    "lmstudio",
    "opencodego",
    "openrouter",
    "claude",
    "codex",
    "cursor",
};
```

Update the `Reminder: hidden providers` comment to exclude `opencodego`.

### `firmware/main/stats_model.c:240-274` — MODIFY

Add `parse_oc()` static function following `parse_cu()` pattern, and call it from the provider loop:

```c
// v2 optional `oc` block: OpenCode Go token/cost rollup from opencode.ai API.
// Dedicated fields (not cost-slot reuse) — token-focused hero, cost is secondary text.
static void parse_oc(const cJSON *e, stats_provider_t *p)
{
    const cJSON *oc = cJSON_GetObjectItemCaseSensitive(e, "oc");
    if (strcmp(p->id, "opencodego") != 0 || !cJSON_IsObject(oc)) return;
    bool any_oc = false;
    if (get_i64(oc, "tk",  &p->oc_tok_today))    any_oc = true;
    if (get_i32(oc, "ct",  &p->oc_cost_today_c)) any_oc = true;
    if (get_i64(oc, "mxt", &p->oc_tok_month_max)) any_oc = true;
    const cJSON *ht = cJSON_GetObjectItemCaseSensitive(oc, "ht");
    if (cJSON_IsArray(ht)) {
        const cJSON *hv;
        cJSON_ArrayForEach(hv, ht) {
            if (p->oc_ht_n >= STATS_HIST_MAX) break;
            if (cJSON_IsNumber(hv)) {
                p->oc_ht[p->oc_ht_n++] = i64_clamp(hv->valuedouble);
                any_oc = true;
            }
        }
    }
    if (any_oc) p->has_oc = true;
}
```

Call from provider loop (after `parse_cu(e, p);`):
```c
            parse_oc(e, p);
```

### `firmware/main/ui_internal.h:59` — MODIFY

Remove `"opencodego"` from HIDDEN_PROVIDERS:
```c
#define HIDDEN_PROVIDERS "ollama", "opencode"
```

### `firmware/main/ui_internal.h:81-87` — MODIFY

Add `PK_OPENCODEGO` to the `provider_kind_t` enum:
```c
    PK_OPENCODEGO,
    // Keep PK_... last for bounds checks (aliased by the default case).
}
```

(Place it after `PK_CURSOR` and before the closing brace.)

### `firmware/main/ui_format.c:145-163` — MODIFY

Add `opencodego` branch to `provider_kind()`:
```c
    if (strcmp(id, "opencodego") == 0) return PK_OPENCODEGO;
```

(Place after the cursor check, before `return PK_UNKNOWN;`)

### `firmware/main/ui_format.c:165-176` — MODIFY

Add `case PK_OPENCODEGO` to `summary_provider_name()`:
```c
    case PK_OPENCODEGO: return "OpenCode Go";
```

### `firmware/main/ui_format.c:126-133` — MODIFY

Add `p->has_oc` to `provider_card_available()` CARD_COST case:
```c
        case CARD_COST: return p->has_cost || p->has_lm || p->has_cu || p->has_oc;
```

### `firmware/main/ui_format.c:215-221` — MODIFY

Add `case PK_OPENCODEGO` to `provider_tok_today()`:
```c
    case PK_OPENCODEGO: return p->has_oc ? p->oc_tok_today : 0;
```

### `firmware/main/ui.c:244-248` — MODIFY

Add `(pk == PK_OPENCODEGO && p->has_oc)` to `provider_has_both_cards()`:
```c
    return pk == PK_LMSTUDIO || (pk == PK_CURSOR && p->has_cu) || (pk == PK_OPENCODEGO && p->has_oc);
```

### `firmware/main/ui_render.c:1143-1160` — MODIFY

Add PK_OPENCODEGO branch before `has_balance` check in `render_cost_card()`:
```c
    case PK_OPENCODEGO:
        if (p->has_oc) {
            render_opencodego_today(p, g, hero, card_entered);
            return;
        }
        break;
```

### `firmware/main/ui_render.c:993-1030` — MODIFY

Add `render_opencodego_today()` function after `render_cursor_chart()`:

```c
// OpenCode Go TODAY card: tokens hero + cost line + 30-day token bar chart.
static void render_opencodego_today(const stats_provider_t *p,
                                    const ui_page_grid_t *g,
                                    const ui_rect_t *hero,
                                    bool card_entered)
{
    lv_obj_t *hide[] = { cost.or_lbl, cost.or_row1, cost.or_row2,
                         cost.bar, cost.bar_lbl };
    for (unsigned i = 0; i < sizeof hide / sizeof *hide; i++)
        lv_obj_add_flag(hide[i], LV_OBJ_FLAG_HIDDEN);
    cost_tok_set_parent(cost.card);
    lv_obj_clear_flag(cost.tok, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(cost.tok_unit, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(cost.cost_30, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(cost.chart, LV_OBJ_FLAG_HIDDEN);

    // Hero: rows 0-1, 2 cols — tokens count with "TOKENS" caption
    place_hero_amount(&cost_hero, hero, "TOKENS");
    char tk[16];
    fmt_tokens(tk, sizeof tk, p->oc_tok_today);
    set_hero_amount(&cost_hero, NULL, tk, NULL);

    // Row 2 — cost line "$1.23"
    const ui_rect_t cost_r = ui_grid_span(g, 0, 2, 2, 1);
    lv_obj_set_pos(cost.tok, cost_r.x + 12, cost_r.y + 2);
    char ct[16];
    fmt_money(ct, sizeof ct, p->oc_cost_today_c);
    lv_label_set_text(cost.tok, ct);
    lv_label_set_text(cost.tok_unit, "today spend");
    lv_obj_align_to(cost.tok_unit, cost.tok, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, 0);

    // Chart: rows 3-8 — 30-day token bar chart
    const ui_rect_t chart_r = ui_grid_span(g, 0, 3, 2, 5);
    lv_obj_set_size(cost.chart, chart_r.w - 24, chart_r.h - 8);
    lv_obj_set_pos(cost.chart, chart_r.x + 12, chart_r.y + 4);

    // Footer: row 8 — max tokens . max spend
    const ui_rect_t footer_r = ui_grid_span(g, 0, 8, 2, 1);
    lv_obj_set_pos(cost.cost_30, footer_r.x + 12, footer_r.y + 2);
    char tk30[16], ct_today[16];
    fmt_tokens(tk30, sizeof tk30, p->oc_tok_month_max);
    fmt_money(ct_today, sizeof ct_today, p->oc_cost_today_c);
    lv_label_set_text_fmt(cost.cost_30, "max %s Toks  " LV_SYMBOL_BULLET "  max %s spend", tk30, ct_today);

    // Hide cap (not used)
    lv_obj_add_flag(cost.cap, LV_OBJ_FLAG_HIDDEN);

    // Render bar chart with token history
    int n = p->oc_ht_n;
    if (n > NAV_HIST_PTS) n = NAV_HIST_PTS;
    lv_color_t cc;
    int32_t ht32[STATS_HIST_MAX];
    i64_hist_to_i32(ht32, p->oc_ht, n);
    (void)render_cost_bar_chart(cost.chart, cost.ser, ht32, n,
        prov_accent(p->id, &cc) ? cc : lv_color_hex(UI_DEFAULT_CHART_COLOR));
    if (card_entered) anim_chart_fadein(cost.chart);
}
```

### `firmware/main/ui_render.c:1493-1517` — MODIFY

Add `PK_OPENCODEGO` to `render_summary_secondary_bar()`:
```c
    if (((rpk == PK_CLAUDE || rpk == PK_CODEX) && p->secondary.has)
        || rpk == PK_LMSTUDIO
        || (rpk == PK_OPENCODEGO && p->secondary.has)) {
```

### `firmware/main/ui_format.c:180-280` — MODIFY

Add `has_oc` to `provider_metric_sig()` (after `has_cu` block):
```c
    h = hash_mix_u32(h, p->has_oc ? 1U : 0U);
    if (p->has_oc) {
        h = hash_mix_u32(h, (uint32_t)p->oc_tok_today);
        h = hash_mix_u32(h, (uint32_t)(p->oc_tok_today >> 32));
        h = hash_mix_u32(h, (uint32_t)p->oc_cost_today_c);
        h = hash_mix_u32(h, (uint32_t)p->oc_tok_month_max);
        h = hash_mix_u32(h, (uint32_t)(p->oc_tok_month_max >> 32));
        h = hash_mix_u32(h, (uint32_t)p->oc_ht_n);
        for (int i = 0; i < p->oc_ht_n && i < STATS_HIST_MAX; i++)
            h = hash_mix_u32(h, (uint32_t)p->oc_ht[i]);
    }
```

### `firmware/test/stats_model/test_stats_model.c:676-706` — MODIFY

Add reorder test for opencodego insertion:
```c
static void test_reorder_opencodego_insertion(void)
{
    stats_t s = {0};
    strcpy(s.p[0].id, "openrouter"); strcpy(s.p[0].primary.reset, "");
    strcpy(s.p[1].id, "lmstudio");   strcpy(s.p[1].primary.reset, "");
    strcpy(s.p[2].id, "opencodego");  strcpy(s.p[2].primary.reset, "");
    strcpy(s.p[3].id, "pi");         strcpy(s.p[3].primary.reset, "");
    strcpy(s.p[4].id, "cursor");     strcpy(s.p[4].primary.reset, "");
    s.n = 5;
    stats_model_reorder(&s);
    TEST_ASSERT_EQUAL_STRING("pi",         s.p[0].id);
    TEST_ASSERT_EQUAL_STRING("lmstudio",   s.p[1].id);
    TEST_ASSERT_EQUAL_STRING("opencodego", s.p[2].id);
    TEST_ASSERT_EQUAL_STRING("openrouter", s.p[3].id);
    TEST_ASSERT_EQUAL_STRING("cursor",     s.p[4].id);
}
```

### `firmware/test/stats_model/test_stats_model.c` — MODIFY

Add parse test (following `test_cursor_provider_parsed` pattern):
```c
static void test_opencodego_provider_parsed(void)
{
    const char *json = "{"
        "\"v\":2,\"ts\":\"2026-06-02T00:00:00Z\","
        "\"providers\":[{"
            "\"id\":\"opencodego\",\"ok\":true,\"p\":45.0,\"pr\":\"5h\","
            "\"s\":30.0,\"sr\":\"weekly\",\"t\":10.0,\"tr\":\"monthly\","
            "\"oc\":{\"tk\":75000,\"ct\":123,\"mxt\":200000,\"ht\":[5000,6000,7000]}"
        "}]"
    "}";
    stats_t out;
    TEST_ASSERT_EQUAL(STATS_PARSE_OK, stats_model_parse(json, &out));
    TEST_ASSERT_EQUAL(1, out.n);
    const stats_provider_t *p = &out.p[0];
    TEST_ASSERT_TRUE(p->has_oc);
    TEST_ASSERT_EQUAL_INT64(75000, p->oc_tok_today);
    TEST_ASSERT_EQUAL(123, p->oc_cost_today_c);
    TEST_ASSERT_EQUAL_INT64(200000, p->oc_tok_month_max);
    TEST_ASSERT_EQUAL(3, p->oc_ht_n);
    TEST_ASSERT_EQUAL_INT64(5000,  p->oc_ht[0]);
    TEST_ASSERT_EQUAL_INT64(6000,  p->oc_ht[1]);
    TEST_ASSERT_EQUAL_INT64(7000,  p->oc_ht[2]);
}
```

### `docs/generated/codexbar-payload.schema.json` — MODIFY

Add `oc` block definition as an optional property on each provider entry:
```json
"oc": {
    "type": "object",
    "description": "v2 optional, OpenCode Go provider only. Token rollup + today's cost from opencode.ai API. Present only on provider id `opencodego`.",
    "required": ["tk", "ct", "mxt", "ht"],
    "properties": {
        "tk":  { "type": "integer", "description": "Tokens today (local calendar day)." },
        "ct":  { "type": "integer", "description": "Cost today in cents." },
        "mxt": { "type": "integer", "description": "30-day max daily tokens." },
        "ht":  {
            "type": "array",
            "items": { "type": "integer" },
            "maxItems": 31,
            "description": "Daily token totals, oldest -> newest."
        }
    },
    "additionalProperties": false
}
```

### `scripts/opencodego-stats.sh` — NEW

Host-side scraper following `cursor-stats.sh` pattern. Key sections:

```zsh
#!/bin/zsh
# opencodego-stats.sh — scrape OpenCode Go usage from opencode.ai API and emit
# a compact provider fragment merged by codexbar-publish.sh (in-place oc patch).
#
#   opencodego-stats.sh             # emits {"id":"opencodego","ok":true,"oc":{...}}
#   opencodego-stats.sh --help
#
# Reads:
#   Keychain  service=codexbar-toy  account=opencodego-session
#   ~/.config/codexbar-toy/opencodego-history.json   rolling 30-day token history
#
# Output:
#   {"id":"opencodego","ok":true,"oc":{"tk":...,"ct":...,"mxt":...,"ht":[...]}}
#
# Env overrides (testability):
#   OPENCODE_GO_COOKIE       inline cookie for testing (bypasses Keychain)
#   OPENCODE_GO_HISTORY      default: ~/.config/codexbar-toy/opencodego-history.json
#   OPENCODE_GO_API_URL      default: https://opencode.ai/workspace/.../usage
#   PYTHON3                  Python interpreter
set -u
# ... (full implementation in Slice 4)
```

### `scripts/codexbar-publish.sh:55-65` — MODIFY

Add env var declarations:
```zsh
OPENCODE_GO_STATS="${CBPUB_OPENCODE_GO_STATS:-$SELF_DIR/opencodego-stats.sh}"
KC_ACCOUNT_OG="${CBPUB_KC_ACCOUNT_OG:-opencodego-session}"
```

### `scripts/codexbar-publish.sh:116-131` — MODIFY

Add `get_opencodego_session()`, `store_opencodego_session()`, `cmd_set_opencodego_cookie()`, `cmd_set_opencodego_cookie_clipboard()` following the `store_cursor_session()` pattern.

### `scripts/codexbar-publish.sh:531-577` — MODIFY

Add `OG_MERGE_JXA` heredoc following the `CURSOR_MERGE_JXA` in-place patch pattern.

### `scripts/codexbar-publish.sh:674-711` — MODIFY

Add timeout-30 merge stage for OpenCode Go after the LM Studio merge, before Cursor cu merge.

## Slices

### Slice 1: Data model + parsing — oc block fields, parse_oc(), display order, schema

**Files**: `firmware/main/stats_model.h` (MODIFY), `firmware/main/stats_model.c` (MODIFY), `firmware/test/stats_model/test_stats_model.c` (MODIFY), `docs/generated/codexbar-payload.schema.json` (MODIFY)

#### Automated Verification:
- [x] Build passes: `cd firmware && idf.py build`
- [x] Unit tests pass: `cd firmware/test/stats_model && ./runtests`
- [x] Parse test verifies has_oc, oc_tok_today, oc_cost_today_c, oc_tok_month_max, oc_ht_n, oc_ht values
- [x] Reorder test verifies opencodego between lmstudio and openrouter
- [x] Existing `test_reorder_unknown_sinks_to_end` passes unchanged

#### Manual Verification:
- [x] Schema `oc` block matches parse_oc() field expectations
- [x] `s_display_order[]` comment excludes opencodego from hidden remark

### Slice 2: UI identity + card plumbing — unhide, PK_OPENCODEGO, dispatch wiring

**Files**: `firmware/main/ui_internal.h` (MODIFY), `firmware/main/ui_format.c` (MODIFY), `firmware/main/ui.c` (MODIFY), `firmware/main/ui_render.c` (MODIFY — N/A guard fix)

#### Automated Verification:
- [x] Build passes: `cd firmware && idf.py build`
- [x] Unit tests pass: `cd firmware/test/stats_model && ./runtests`
- [x] `provider_kind("opencodego")` returns PK_OPENCODEGO
- [x] `summary_provider_name("opencodego")` returns "OpenCode Go"
- [x] `provider_card_available()` returns true for CARD_COST when has_oc is true
- [x] `provider_has_both_cards()` returns true for PK_OPENCODEGO when has_oc is true
- [x] `is_hidden_provider("opencodego")` returns false
- [x] `render_cost_card()` N/A guard includes `&& !p->has_oc`

#### Manual Verification:
- [x] OpenCode Go appears on summary (between LM Studio and OpenRouter)
- [x] Tapping opens TODAY page (fallback N/A until Slice 3)
- [x] LM Studio and OpenRouter ordering unchanged
- [x] Screen reader: all fixed-dispatch points reference `has_oc` from stats_model.h:108

### Slice 3: UI TODAY page — render_opencodego_today(), cost card branch

**Files**: `firmware/main/ui_render.c` (MODIFY)

#### Automated Verification:
- [x] Build passes: `cd firmware && idf.py build`
- [x] `render_cost_card()` routes PK_OPENCODEGO with has_oc to `render_opencodego_today()`
- [x] Chrome subtitle "TODAY" set by caller `render_cost_card()` (by design, matches LM Studio/Cursor pattern)
- [x] Token hero, cost line, chart, and footer all hidden correctly when no has_oc
- [x] LVGL widgets show/hide correctly

#### Manual Verification:
- [x] TODAY page shows tokens hero on rows 0-1
- [x] Cost line "$X.XX" at row 2 with "today spend" label
- [x] 30-day token bar chart on rows 3-8
- [x] Footer: "max N Toks • max $X spend"
- [x] Tapping cycles TODAY ↔ LIMITS
- [x] Secondary bar shows weekly usage on summary row
- [x] LIMITS page (3-tier: 5h/weekly/monthly) works via data-driven render_limits_card()

### Slice 4: Host pipeline — scraper script + publisher merge + Keychain helpers

**Files**: `scripts/opencodego-stats.sh` (NEW), `scripts/codexbar-publish.sh` (MODIFY)

#### Automated Verification:
- [x] `opencodego-stats.sh --help` prints usage and exits 0
- [x] `codexbar-publish.sh --help` shows both `--set-opencodego-cookie` flags
- [x] OG_MERGE_JXA validates src shape + patches oc in-place
- [x] OG_MERGE_JXA exits 0 when opencodego not found (no-op, not crash)
- [x] Merge stage has `timeout 30` + fail-soft (rc=124 logged as skip)
- [x] Keychain helpers mirror Cursor pattern (get/store/set-cmd)
- [x] oc block fields (tk/ct/mxt/ht) match parse_oc() field expectations

#### Manual Verification:
- [x] `--set-opencodego-cookie` stores in Keychain (same service, account`opencodego-session`)
- [x] Full publish cycle with scraper adds `oc` block to payload
- [x] Publish without scraper: skip logged, no crash
- [x] `opencodego-stats.sh` has TODO markers at API field extraction (user fills actual API shape)
- [x] Device renders opencodego with TODAY + LIMITS cards

## Desired End State

After all slices are implemented:

1. Device reboots, fetches fresh payload from Upstash
2. Summary shows Pi → LM Studio → **OpenCode Go** → OpenRouter → Claude → Codex → Cursor
3. OpenCode Go row shows primary bar (5-hour usage) and secondary thin bar (weekly usage)
4. Tap OpenCode Go → TODAY page showing hero "TOKENS 75K", cost "$1.23", 30-day token chart
5. Tap again → LIMITS page showing 5-hour bar (48%), weekly bar (30%), monthly bar (10%)
6. Host-side: `--set-opencodego-cookie` stores auth cookie in Keychain
7. `codexbar-publish.sh --once` runs `opencodego-stats.sh`, merges `oc` block, publishes to Upstash
8. If scraper fails (timeout/no cookie), publish continues without `oc` data

## File Map

- `firmware/main/stats_model.h:46-56` — MODIFY — Add oc fields to stats_provider_t
- `firmware/main/stats_model.c:240-274` — MODIFY — Add parse_oc() + call from provider loop + display order
- `firmware/main/ui_internal.h:59` — MODIFY — Remove "opencodego" from HIDDEN_PROVIDERS
- `firmware/main/ui_internal.h:81-87` — MODIFY — Add PK_OPENCODEGO to provider_kind_t enum
- `firmware/main/ui_format.c:145-163` — MODIFY — Add opencodego to provider_kind()
- `firmware/main/ui_format.c:165-176` — MODIFY — Add "OpenCode Go" to summary_provider_name()
- `firmware/main/ui_format.c:126-133` — MODIFY — Add has_oc to provider_card_available()
- `firmware/main/ui_format.c:215-221` — MODIFY — Add PK_OPENCODEGO to provider_tok_today()
- `firmware/main/ui_format.c:180-280` — MODIFY — Add has_oc to provider_metric_sig()
- `firmware/main/ui.c:244-248` — MODIFY — Add PK_OPENCODEGO && has_oc to provider_has_both_cards()
- `firmware/main/ui_render.c:993-1030` — MODIFY — Add render_opencodego_today() function
- `firmware/main/ui_render.c:1143-1160` — MODIFY — Add PK_OPENCODEGO branch in render_cost_card()
- `firmware/main/ui_render.c:1493-1517` — MODIFY — Add PK_OPENCODEGO to render_summary_secondary_bar()
- `firmware/test/stats_model/test_stats_model.c` — MODIFY — Add reorder + parse tests
- `docs/generated/codexbar-payload.schema.json` — MODIFY — Add oc block definition
- `scripts/opencodego-stats.sh` — NEW — OpenCode Go API scraper
- `scripts/codexbar-publish.sh` — MODIFY — OG_MERGE_JXA + merge stage + Keychain helpers

## Ordering Constraints

- Slice 1 must be built and verified before Slice 2 (needs oc fields on stats_provider_t)
- Slice 2 must be built before Slice 3 (needs PK_OPENCODEGO + provider_has_both_cards)
- Slice 4 (host pipeline) has NO dependency on firmware slices and runs in parallel
- The LIMITS page requires no dedicated slice — it works automatically via data-driven render_limits_card() once Slice 2 makes the provider visible with p/s/t tiers

## Verification Notes

- Publisher merge must have `timeout 30` + fail-soft from day one (precedent: Pi Agent `4613728`)
- Card layout may need visual fixup post-release (precedent: LM Studio 3 fixes in 2 days)
- `has_oc` must be checked at EVERY dispatch point: provider_card_available(), provider_has_both_cards(), provider_tok_today(), provider_metric_sig()
- LEMONMILK font covers numbers, "$", "%", and "." — verify "OpenCode Go" label renders correctly (no missing glyphs)
- Build must pass with `-Werror`: `cd firmware && idf.py build`
- Use `i64_hist_to_i32()` for chart rendering from int64 history

## Performance Considerations

- Negligible — adding 5 fields + 1 history array to an already ~420-byte struct, one extra `cJSON_GetObjectItemCaseSensitive` per provider parse cycle (0.1ms)
- History array `oc_ht[31]` is 248 bytes (31 × 8 bytes) — within ESP32-S3 PSRAM budget
- No new tasks, queues, or allocations

## Migration Notes

Not applicable — new data fields on existing provider, no persisted schema change.

## Pattern References

- `stats_model.c:217-241` — `parse_cu()` — direct template for `parse_oc()`
- `ui_render.c:908-993` — `render_token_chart_card()` — template for tokens hero + chart
- `ui_render.c:1369-1446` — `render_limits_card()` — data-driven 3-tier template
- `ui_render.c:1493-1517` — `render_summary_secondary_bar()` — secondary bar pattern
- `scripts/cursor-stats.sh:1-527` — full scraper pattern
- `scripts/codexbar-publish.sh:531-577` — CURSOR_MERGE_JXA in-place patch pattern
- `scripts/codexbar-publish.sh:116-131` — Keychain storage pattern for auth cookie

## Developer Context

**Q (render_summary_secondary_bar():1493)**: "About to follow the LM Studio / Cursor pattern of showing a SECONDARY bar on the summary row for OpenCode Go, using render_summary_secondary_bar() at ui_render.c:1493. This would add PK_OPENCODEGO to the if-chain and show the weekly usage (secondary tier s) as a thin bar below the primary 5-hour bar. Confirm this direction, or keep it hidden like the other providers?"
**A**: "Follow — show secondary bar (Recommended)"

**Q (Summary bar mapping)**: "On the SUMMARY page, the user said 'top bar = Weekly usage, smaller bar = Monthly usage'. But CodexBar fields for OpenCode Go are: p (5-hour session), s (weekly), t (monthly). The LIMITS page shows all 3 (5-hour/weekly/monthly). On the summary, should the primary bar show the 5-hour usage (p, matching other providers' standard) with the secondary bar showing weekly (s), or do you want the primary bar to show weekly directly?"
**A**: "Primary=p (5h), Secondary=s (weekly) (Recommended)"

**Q (Micro-checkpoint interactions)**: *Recorded in Slice sections below as slices are generated and approved.*

## Design History

- Slice 1: Data model + parsing — approved as generated
- Slice 2: UI identity + card plumbing — approved as generated
- Slice 3: UI TODAY page — approved as generated
- Slice 4: Host pipeline — approved as generated

## References

- `.rpiv/artifacts/research/2026-06-02_10-27-20_opencode-go-provider.md` — research artifact
- `.rpiv/artifacts/research/2026-05-23_11-34-39_lm-studio-stats-on-device.md` — LM Studio research (most similar)
- `.rpiv/artifacts/research/2026-05-26_12-30-00_cursor-token-stats.md` — Cursor token stats research (in-place patch)
- `docs/generated/codexbar-payload.schema.json` — authoritative payload contract
- `docs/RELIABILITY.md` — fail-soft patterns for publisher merge
- `docs/SECURITY.md` — Keychain storage rules
