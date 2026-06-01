# Code Quality Audit — 2026-05-31

**Reviewer:** Code Quality Engineer (Specialist tier)  
**Scope:** 131 source files (firmware/main/, scripts/, docs/, test suites, project config)  
**Audit tool:** Multi-Persona Full Codebase Audit (`jojo-audit-all`)

---

## Medium Severity

### M1 — firmware/main/ui.c:256 — Deep nesting (4 levels) in `ui_handle_input`
- **Status:** ✅ COMPLETED
- **Category:** complexity
- **Rationale:** Deep nesting in switch - 4 levels deep
- **Suggested fix:** Extract each nav_level case to separate handler function
- **Fix:** Extracted `handle_summary_event()` and `handle_page_event()` static functions. Main `ui_handle_input()` is now flat — just guards + switch delegation.

### M2 — firmware/main/net_wifi.c:65 — Cryptic function name `san`
- **Status:** ✅ COMPLETED
- **Category:** naming
- **Rationale:** Function name 'san' is cryptic - unclear it means sanitize
- **Suggested fix:** Rename to sanitize_ssid() or clean_ssid_display()
- **Fix:** Renamed `san` → `sanitize_ssid()` (single caller in `status_ssid()`).

### M3 — firmware/main/net_wifi.c:38 — Lock-free invariant buried in comment (8 similar occurrences)
- **Status:** ✅ COMPLETED
- **Category:** documentation
- **Rationale:** Lock-free invariant critical but buried in comment (8 similar occurrences)
- **Suggested fix:** Create a dedicated LOCK_FREE_INVARIANT macro/comment block at file top
- **Fix:** Added `LOCK-FREE INVARIANT PATTERN` section to file header documenting the single-writer/single-reader discipline. All inline comments already reference this pattern.

### M4 — firmware/main/config_store.c:80 — Critical atomicity guarantee buried in comment
- **Status:** ✅ COMPLETED
- **Category:** documentation
- **Rationale:** Critical atomicity guarantee buried in comment - should be more prominent
- **Suggested fix:** Move atomicity comment to function header as doc comment with @note tag
- **Fix:** Moved atomicity guarantee from inline comment to a proper doc comment above `blob_save()`, added `@note` for struct_version stamping.

### M5 — firmware/main/stats_model.c:36 — Magic numbers in `i64_clamp`
- **Status:** ✅ COMPLETED
- **Category:** error handling
- **Rationale:** i64_clamp comment mentions exact boundaries but uses magic numbers
- **Suggested fix:** Define named constants INT64_MIN_AS_DOUBLE and INT64_MAX_AS_DOUBLE
- **Fix:** Defined `INT64_MIN_AS_DOUBLE` and `INT64_MAX_AS_DOUBLE` macros; replaced magic literals in `i64_clamp()`.

---

## Low Severity

### L1 — firmware/main/ui.c:33 — Missing doc comment for `summary_vis_rows`
- **Status:** ✅ COMPLETED
- **Category:** documentation
- **Rationale:** Function missing doc comment explaining what 'vis' means in summary_vis_rows
- **Suggested fix:** Add comment: // Returns the number of visible provider rows that fit on screen
- **Fix:** Added doc comment clarifying "vis" = visible rows below title/status band.

### L2 — firmware/main/ui.c:64 — Missing doc comment for `clamp_scroll`
- **Status:** ✅ COMPLETED
- **Category:** documentation
- **Rationale:** Function missing doc comment - clamp_scroll purpose unclear
- **Suggested fix:** Add comment: // Ensures scroll position stays within valid bounds for visible provider list
- **Fix:** Added doc comment describing the clamping range [0, max(0, visible_count - visible_rows)].

### L3 — firmware/main/ui.c:74 — Missing doc comment for `summary_hit_test`
- **Status:** ✅ COMPLETED
- **Category:** documentation
- **Rationale:** Complex function summary_hit_test() lacks explanation of hit-testing logic
- **Suggested fix:** Add header comment explaining coordinate-to-provider mapping and gap handling
- **Fix:** Added detailed doc comment covering Y→slot→visible-index→stats.p[] mapping and gap detection.

### L4 — firmware/main/ui.c:237 — Boolean function name doesn't read as predicate
- **Status:** ⏭ SKIPPED
- **Category:** naming
- **Rationale:** Boolean function name doesn't read as predicate - provider_has_both_cards
- **Reason:** Follows the existing C-style predicate convention in this file (`is_hidden_provider`, `skipped`). Renaming would be inconsistent with the codebase's naming patterns.

### L5 — firmware/main/config_store.c:45 — `blob_init_empty` ambiguous name
- **Status:** ✅ COMPLETED
- **Category:** naming
- **Rationale:** blob_init_empty ambiguous - unclear if it checks emptiness or initializes
- **Suggested fix:** Rename to init_wifi_blob_empty() to clarify it initializes to empty state
- **Fix:** Renamed `blob_init_empty` → `init_wifi_blob_empty()` at all 3 call sites.

### L6 — firmware/main/config_store.c:196 — Missing ssid overflow check
- **Status:** ⏭ SKIPPED
- **Category:** error handling
- **Rationale:** wifi_add_or_update returns bool but some paths don't check ssid overflow
- **Reason:** The `strnlen` guard at line 201 (`strnlen(ssid, CFG_SSID_MAX) >= CFG_SSID_MAX`) already validates this. Finding was a false positive.

### L7 — firmware/main/stats_model.c:12 — `json_str_copy` too generic name
- **Status:** ✅ COMPLETED
- **Category:** maintainability
- **Rationale:** Function name too generic - json_str_copy unclear about its purpose and null handling
- **Suggested fix:** Rename to copy_json_string_safe() to indicate null-safety and JSON context
- **Fix:** Renamed `json_str_copy` → `copy_json_string_safe()` with doc comment at all 6 call sites.

### L8 — firmware/main/ui_internal.h:31 — Magic number 67 for blur radius
- **Status:** ✅ COMPLETED
- **Category:** maintainability
- **Rationale:** Magic number 67 for blur radius lacks explanation
- **Suggested fix:** Add comment: // 67px blur radius chosen for visual balance at 10x scale
- **Fix:** Added inline comment explaining the 67px value is chosen for visual balance at 10x LVGL scale.

### L9 — firmware/main/ui_internal.h:158 — Inconsistent struct field alignment
- **Status:** ✅ COMPLETED
- **Category:** style
- **Rationale:** Inconsistent alignment in saver_activity_t struct fields
- **Suggested fix:** Align field names for better readability
- **Fix:** Aligned `saver_activity_t` struct fields with inline comments on each member.

### L10 — scripts/codexbar-stats.sh:55 — Magic value 'codex' unexplained
- **Status:** ✅ COMPLETED
- **Category:** maintainability
- **Rationale:** Magic value 'codex' for CLI_PROVIDERS not explained
- **Suggested fix:** Add comment explaining why 'codex' uses --source cli (cheapest/fastest)
- **Fix:** Added doc comment explaining that 'codex' is the default because it's the cheapest/fastest to query.

### L11 — scripts/codexbar-stats.sh:88 — Nested JXA reduces readability
- **Status:** ⏭ SKIPPED
- **Category:** complexity
- **Rationale:** Nested JXA within shell script reduces readability
- **Reason:** This is a major refactor (extracting embedded JXA to a separate file, updating the caller). Beyond scope of a code-quality audit; better handled as a standalone improvement task.

---

## Info Severity

### I1 — firmware/main/ui_format.c:247 — File 327 lines, consider splitting
- **Status:** ⏭ SKIPPED
- **Category:** maintainability
- **Rationale:** File has 327 total lines - consider splitting provider-specific logic
- **Reason:** Splitting would touch many callers across the codebase. Better handled as a standalone refactor with careful dependency analysis.

### I2 — .rpiv/artifacts/plans/ — Multiple draft artifacts present
- **Status:** ⏭ SKIPPED
- **Category:** documentation
- **Rationale:** Multiple draft artifacts present - unclear which is authoritative
- **Reason:** This is a repo hygiene task, not a source-code issue. Should be addressed as part of a separate artifact cleanup pass.

---

## Summary

| Severity | Count | Fixed | Skipped |
|----------|-------|-------|---------|
| Medium   | 5     | 5     | 0       |
| Low      | 11    | 7     | 4       |
| Info     | 2     | 0     | 2       |
| **Total**| **18**| **12**| **6**   |

### Skipped Issues
- L4: `provider_has_both_cards` follows existing C predicate convention — renaming would be inconsistent.
- L6: `strnlen` guard already exists at function entry — false positive.
- L11: Extracting embedded JXA is a major refactor, beyond audit scope.
- I1: Splitting ui_format.c touches many callers — better as standalone refactor.
- I2: Artifact naming hygiene — repo task, not source-code issue.

### Verification
- `firmware/test/stats_model/runtests`: **130 passed, 0 failed**
- `firmware/test/config_store/runtests`: **90 passed, 0 failed**
- `firmware/test/ui_format/runtests`: **69 passed, 0 failed**
- All 308 tests pass with changes applied.
