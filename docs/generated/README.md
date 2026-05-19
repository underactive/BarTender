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
