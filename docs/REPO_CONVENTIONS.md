# Repo Conventions

Orientation for working in this repo. See [CODE_STYLE.md](CODE_STYLE.md) for
formatting specifics and [../ARCHITECTURE.md](../ARCHITECTURE.md) for the
system layout.

- **Language:** C (ESP-IDF firmware) for the device; Python 3 + zsh for host
  tooling under `scripts/`. Prefer boring, dependency-light solutions —
  flash, RAM, and the absence of a package manager on-device all favor the
  standard toolchain over new libraries.
- **Boundaries:** treat everything crossing a system edge as untrusted and
  validate it there. The Upstash response is parsed and clamped in
  `stats_model.c` (range/NaN guards, forward-version gate); HTTP status and
  TLS are enforced in `upstash.c`; NVS blobs are validated in
  `config_store.c`. Interior code trusts the parsed `stats_t`.
- **Tests:** host-side unit tests live in `firmware/test/<module>/` and build
  with a plain `cc` + Makefile (no device required) — run `./runtests`. The
  data parser (`stats_model`) and the WiFi-LRU/config logic are covered this
  way; parser tests feed real sample payloads from `docs/references/`. Add a
  test alongside any change to parsing or the WiFi-LRU model.
- **Logging:** ESP-IDF `ESP_LOGx` with a per-file `TAG`. Levels: `E` for
  faults, `W` for degraded-but-recoverable, `I` for lifecycle milestones, `D`
  for input/gesture traces. Never log secrets — the Upstash bearer token must
  not appear in any log line.
- **Naming:** `snake_case` files and functions; `UPPER_SNAKE_CASE` macros;
  file-scope statics prefixed `s_`; lock-required helpers suffixed `_locked`.
- **File size:** no hard line limit, but a translation unit should own one
  concern. When a file accretes several (as `ui.c` did), split it into
  cohesive modules sharing an `*_internal.h` rather than letting it sprawl.
- **Imports / dependencies:** keep the dependency direction acyclic and flowing
  toward the lower layers (display/net/store) — UI and fetch depend on the
  model and drivers, not vice versa. Vendored ESP/LVGL components live under
  `firmware/managed_components/` and are not edited by hand.
