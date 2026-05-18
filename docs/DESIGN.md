# Output & UI Conventions

Conventions for user-facing output and interface design. Adapt these to your
project's output medium (CLI, web UI, API responses, etc.).

## Output principles

<!-- Define your output conventions here. Examples for a CLI tool:

1. **Structured by default.** Output JSON when piped (--json flag or
   stdout is not a TTY). Human-readable tables when interactive.
2. **Progressive detail.** Default output is a summary. --verbose adds
   detail. --debug adds raw internal output.
3. **Color is informational.** Use color to encode severity or category,
   not for decoration. Respect NO_COLOR env var.
4. **No spinners in non-TTY.** Progress indicators only in interactive mode.

Adapt these for your medium — the underlying idea is progressive disclosure:
show the minimum useful output by default, let users drill down. -->

## Display format

<!-- Define how your primary data type is displayed to users. Example:

```
[severity] category — file:line
  rationale (truncated to 1 line)
  ▸ suggested action (if available)
```

Group by relevant dimension, sort by priority within each group. -->

## Future considerations

<!-- Note UI/UX directions the project may grow into. Example:

- Follow progressive-disclosure pattern: summary first, drill down
- Attribution should be visible but not dominant
- Confidence or agreement level is the primary visual signal -->
