# Security

Security boundaries must be clearly defined and enforced, especially for
systems that process external input or integrate with third-party services.

## Threat model

1. **External input is untrusted.** Data from APIs, user input, third-party
   services, or file uploads may contain injection attempts, malformed
   payloads, or malicious content. All external data is treated as untrusted
   strings until validated.

2. **User data may be sensitive.** The system must not leak user data to
   unintended destinations. Integrations should use official APIs and
   authenticated channels — no intermediary services.

3. **Credentials are user-managed.** The system does not store or manage API
   keys or secrets directly. External services handle their own auth. The
   system only references credentials via environment variables or secure
   vaults.

## Rules

- **No `eval` or dynamic code execution on external input.** Ever.
- **No shell interpolation of external data.** Use array-form exec, not
  string concatenation, when external data appears near shell invocations.
- **File paths from external input are validated** against an allowed root
  directory. No path traversal.
- **Operations are read-only by default.** Destructive or write operations
  require explicit opt-in from the user.
- **Temporary resources are disposable.** They are created in scoped
  directories and cleaned up after each operation.
- **No network calls triggered by external data.** If external input contains
  a URL, that URL is displayed or logged — not fetched.

## Sensitive files

The system should warn (not block) if operations touch:
- `.env`, `.env.*`
- Files matching `*secret*`, `*credential*`, `*token*`
- CI/CD configuration (`.github/workflows/`, `.gitlab-ci.yml`)
- Package lockfiles (flag for human review)
