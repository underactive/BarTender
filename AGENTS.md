# {{PROJECT_NAME}} — Agent Map

{{PROJECT_DESCRIPTION}}

## Quick orientation

| What you need              | Where to look                          |
|----------------------------|----------------------------------------|
| Domain model & layering    | [ARCHITECTURE.md](ARCHITECTURE.md)     |
| Design principles          | [docs/design-docs/core-beliefs.md](docs/design-docs/core-beliefs.md) |
| Product specs & user flows | [docs/product-specs/index.md](docs/product-specs/index.md) |
| Current execution plans    | [docs/exec-plans/active/](docs/exec-plans/active/) |
| Completed plans & history  | [docs/exec-plans/completed/](docs/exec-plans/completed/) |
| Known tech debt            | [docs/exec-plans/tech-debt-tracker.md](docs/exec-plans/tech-debt-tracker.md) |
| Quality grades by domain   | [docs/QUALITY_SCORE.md](docs/QUALITY_SCORE.md) |
| Output & UI conventions    | [docs/DESIGN.md](docs/DESIGN.md)       |
| Reliability requirements   | [docs/RELIABILITY.md](docs/RELIABILITY.md) |
| Security boundaries        | [docs/SECURITY.md](docs/SECURITY.md)   |
| Product sense & taste      | [docs/PRODUCT_SENSE.md](docs/PRODUCT_SENSE.md) |
| Plan index                 | [docs/PLANS.md](docs/PLANS.md)         |
| Reference material         | [docs/references/](docs/references/)   |
| Code style & linting       | [docs/CODE_STYLE.md](docs/CODE_STYLE.md) |
| Repo conventions           | [docs/REPO_CONVENTIONS.md](docs/REPO_CONVENTIONS.md) |
| External integrations      | [docs/EXTERNAL_INTEGRATIONS.md](docs/EXTERNAL_INTEGRATIONS.md) |

## Agent workflow

1. Read this file first for orientation.
2. Check the relevant section in the table above for your task domain.
3. Plan pre-implementation — for complex work (3+ domains, sequential dependencies, non-obvious decisions, or multi-session scope):
   a. Check [docs/exec-plans/active/](docs/exec-plans/active/) for an existing plan.
   b. If none exists, create one using the template in [docs/PLANS.md](docs/PLANS.md) before starting implementation.
   c. Add or update the entry in the [docs/PLANS.md](docs/PLANS.md) index. For simple tasks not requiring a plan, skip to step 6.
4. Plan post-implementation — if a plan was created or referenced in step 3, update the plan document in [docs/exec-plans/active/](docs/exec-plans/active/) to include:
   - **Files changed** — a lightweight index of every file modified (allows plans to be located by scanning file lists without reading full contents)
   - **Implementation summary** — what was actually built, noting any deviations from the original plan
   - **Verification** — steps taken to confirm correctness (tests run, manual checks, build confirmation)
   - **Follow-ups** — remaining work, known limitations, or improvements identified during implementation
5. Post-implementation audit — if a plan was created or referenced in step 3:
   a. Run the subagents in [docs/AUDIT.md](docs/AUDIT.md) **in parallel** to audit all changed files.
   b. Move the plan document from [docs/exec-plans/active/](docs/exec-plans/active/) to [docs/exec-plans/completed/](docs/exec-plans/completed/) and mark it complete in the [docs/PLANS.md](docs/PLANS.md) index.
6. Run `{{TEST_COMMAND}}` before opening a PR. Run `{{LINT_COMMAND}}` to catch structural violations.
7. If you add a new domain or package, update ARCHITECTURE.md.
8. If you add or change a user-facing behavior, update the relevant spec in [docs/product-specs/](docs/product-specs/). If no spec exists for the feature, **create one** using the template at [docs/product-specs/_template.md](docs/product-specs/_template.md) and add it to the index in [docs/product-specs/index.md](docs/product-specs/index.md).
9. If you ship or significantly change a domain, update its grade in [docs/QUALITY_SCORE.md](docs/QUALITY_SCORE.md). Add new domains to the table. Downgrade if you introduced debt; upgrade if you added tests or hardened the module.
10. If you add, rename, or change the shape of a data contract, update the corresponding schema in [docs/generated/](docs/generated/) and its [README](docs/generated/README.md).
11. If you discover tech debt, log it in [docs/exec-plans/tech-debt-tracker.md](docs/exec-plans/tech-debt-tracker.md).
12. If you change user-facing interfaces, defaults, or configuration, update [README.md](README.md) to match. The README is the user's first contact — it must reflect the current state of the project.

## What NOT to do

- Do not put long instructions in this file. Add them to the appropriate doc
  and link from here.
- Do not skip boundary validation because "it's just internal."
- Do not add dependencies that can't be reasoned about from their types alone.
- Do not leave undocumented magic strings or environment variables.
