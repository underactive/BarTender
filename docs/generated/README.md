# Generated Schemas

This directory contains data contract schemas — either auto-generated from
code or hand-maintained to document the shape of key data types.

## Schemas

| Name | Source | Purpose |
|------|--------|---------|
| `codexbar-payload.schema.json` | `scripts/codexbar-stats.sh` + `scripts/codexbar-publish.sh`; consumed by `firmware/main/stats_model.{h,c}` | Upstash payload contract (v2: usage % + extra-usage $ + Claude cost/tokens/30-day history). v1 superset. |

## Keeping schemas in sync

When you add, rename, or change the shape of a data contract:
1. Update or regenerate the corresponding schema in this directory
2. Update this README table
3. Verify that consumers of the schema still work correctly

## Regenerating the schema

`codexbar-payload.schema.json` is **hand-maintained**, not auto-generated.
There is no machine command to regenerate it from `scripts/`. To update it
after a producer-side change:

1. Edit `docs/generated/codexbar-payload.schema.json` by hand, keeping JSON
   Schema draft 2020-12 syntax.
2. Mirror the change in `firmware/main/stats_model.h` (struct + `has_*` flags)
   and `firmware/main/stats_model.c` (parser + clamp).
3. Add or update a sample under `docs/references/*.sample.json` and a parser
   test case under `firmware/test/stats_model/`.
4. Run `cd firmware/test/stats_model && ./runtests` to confirm samples still
   parse against the new schema.
