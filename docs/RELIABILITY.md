# Reliability

Requirements and practices for keeping the system reliable.

## Failure modes to handle

<!-- Replace these examples with your system's actual failure modes.
     Be specific about likelihood and mitigation strategy. -->

| Failure | Likelihood | Mitigation |
|---------|------------|------------|
| External service unavailable | Medium | Retry with backoff, degrade gracefully, surface clear error |
| Malformed input data | High | Validate at boundary, reject gracefully, log raw input |
| Operation timeout | Medium | Configurable timeouts, cancel cleanly, report partial progress |
| Resource exhaustion (disk/memory) | Low | Set limits, clean up aggressively, fail before corrupting state |

## Invariants

<!-- These are guarantees the system must uphold regardless of input.
     Violations of invariants are bugs, not edge cases. -->

1. **Never crash on malformed input.** Parsers and validators must handle any
   input without throwing. Malformed data is logged and rejected gracefully.
2. **Every operation produces a result object**, even if it fails. The result
   contains status, timing, and error context — never a bare exception.
3. **Resource cleanup is guaranteed.** Use try/finally or equivalent patterns.
   Leaked resources (connections, temp files, locks) are reliability bugs.
4. **Operations are idempotent where possible.** Re-running the same operation
   against the same state should be safe, even if results may vary.

## Testing strategy

- **Unit tests:** Core logic, parsers, and validators are unit-tested with
  fixture data. One fixture per input format or edge case.
- **Integration tests:** End-to-end flows against a real (or realistic)
  environment. These are slower and may run only in CI.
- **Chaos fixtures:** Deliberately malformed input (truncated data, mixed
  formats, unexpected types) to verify resilience at system boundaries.
