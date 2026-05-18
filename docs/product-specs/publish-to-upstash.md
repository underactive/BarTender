# Spec: Publish CodexBar usage to Upstash

## User story

As someone building a CodexBar desk toy, I want my Mac to periodically push a
small, non-sensitive usage snapshot to a cloud key, so a standalone ESP32 can
read it over WiFi without ever talking to my Mac directly.

## Acceptance criteria

- [ ] `codexbar-stats.sh --json` emits a compact object
      `{v,ts,providers:[{id,ok,p?,pr?,s?,sr?}]}` for enabled providers only.
- [ ] The JSON contains **no** account email, `loginMethod`, `identity`, org
      name, or `$` cost — ever (whitelist projection, asserted in tests).
- [ ] `codexbar-publish.sh --set-token` stores the Upstash write token in the
      macOS Keychain; the token never appears in argv, logs, or the plist.
- [ ] `codexbar-publish.sh --once` publishes via `POST <url>/set/<key>` with
      `Authorization: Bearer <token>` and exits 0 on HTTP 2xx.
- [ ] `codexbar-publish.sh --install` registers a launchd LaunchAgent that
      republishes every `PUBLISH_INTERVAL` seconds (default 300) and at load;
      `--uninstall` fully removes it.
- [ ] `--status` reports job state, target, token readiness, and recent log.

## Edge cases

| Scenario | Expected behavior |
|----------|-------------------|
| No fresh data (stats rc≠0) | Skip the publish, keep the store's last good value, log it, exit ≠0 |
| Missing Upstash token | Actionable error pointing at `--set-token`, no publish |
| Missing/empty config URL | Clear error naming the config file |
| HTTP non-2xx from Upstash | Log failure + response head, exit ≠0; launchd retries next interval |
| Run under launchd (sparse env) | `CODEXBAR_BIN`/`PATH` baked into the plist at install |
| Ctrl-C / early exit | Temp workdir always cleaned (global EXIT trap) |

## Not in scope

- ESP32 firmware / reading the key (Prompt 3).
- Provisioning the Upstash account/database (user does this out of band).
- Publishing `$` spend or any account-identifying fields.
- Schedulers other than macOS `launchd`.
