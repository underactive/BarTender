# Code Style

This is an ESP-IDF firmware project (C) with a set of Python 3 + zsh tooling
scripts. There is **no automated linter or formatter configured** — style is
maintained by matching the surrounding code. The conventions below describe
what that means in practice.

## Firmware (C — `firmware/main/`, `firmware/test/`)

- **Compiler / standard:** ESP-IDF default toolchain (Xtensa GCC, gnu17). No
  project-level `CMAKE_C_STANDARD` override.
- **Indentation:** 4 spaces, no tabs.
- **Line endings:** LF.
- **Line length:** ~100 columns (soft). Some `snprintf` format strings and
  comments run longer; readability wins over a hard cap.
- **Naming:** `snake_case` for functions, variables, and file names;
  `UPPER_SNAKE_CASE` for macros and constants; module-private file-scope
  symbols prefixed `s_` (e.g. `s_mtx`, `s_shadow`); functions that must be
  called with a lock held are suffixed `_locked`.
- **Comments:** explain the **why**, not the what. Non-obvious decisions and
  invariants carry a short rationale; many cite the audit that motivated them
  (e.g. `// Audit Security§MED: …`). Block comments are fine.
- **Error handling:** check return codes at boundaries. Use `ESP_ERROR_CHECK`
  only where a failure should abort boot; elsewhere log (`ESP_LOGW/E`) and
  degrade gracefully. Never abort/brick on bad *runtime* data — fail safe.
- **Headers:** one public header per module (`foo.h`); shared private decls go
  in `*_internal.h`. Keep the public surface minimal.

## Scripts

- **Python:** Python 3 (`#!/usr/bin/env python3`). Standard library preferred;
  external deps (e.g. `pyserial`, `Pillow`) are documented at the call site.
- **Shell:** `zsh` (`#!/bin/zsh`).
- **Indentation:** 4 spaces (Python); follow PEP 8 spirit, but no enforced
  linter.

## Tooling

There is no `clang-format`, `.editorconfig`, or `ruff`/`black` config in the
repo. If one is added later, document it here and wire it into CI.
