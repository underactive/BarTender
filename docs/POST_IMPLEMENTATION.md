# Post-Implementation Checklist

Run through this list when a plan's implementation is complete, before closing the plan or opening a PR.

## Code quality

- [ ] All tests pass (`{{TEST_COMMAND}}`)
- [ ] Linter passes with no new violations (`{{LINT_COMMAND}}`)
- [ ] No dead code, debug logging, or temporary scaffolding left behind

## Plan hygiene

- [ ] Move the plan file from `docs/exec-plans/active/` to `docs/exec-plans/completed/`
- [ ] Update the index in [docs/PLANS.md](PLANS.md) to reflect the move

## Documentation

- [ ] If a new domain or package was added → update [ARCHITECTURE.md](../ARCHITECTURE.md)
- [ ] If a user-facing behavior was added or changed → update or create the relevant spec in [docs/product-specs/](product-specs/)
- [ ] If a data contract was added, renamed, or reshaped → update [docs/generated/](generated/) and its [README](generated/README.md)
- [ ] If user-facing interfaces, defaults, or config changed → update [README.md](../README.md)

## Quality & debt

- [ ] Update the domain's grade in [docs/QUALITY_SCORE.md](QUALITY_SCORE.md)
  - Upgrade if you added tests or hardened the module
  - Downgrade if you introduced known debt
- [ ] Log any new tech debt in [docs/exec-plans/tech-debt-tracker.md](exec-plans/tech-debt-tracker.md)

## External integrations

- [ ] If a new third-party service or SDK was added → add an entry to [docs/EXTERNAL_INTEGRATIONS.md](EXTERNAL_INTEGRATIONS.md)
