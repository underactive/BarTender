# ui_format host tests

Host-compiled unit tests for `firmware/main/ui_format.c` — the pure formatting,
number, provider-resolution, and color helpers (the Functional Core of the UI
layer). Compiles the **real** `ui_format.c` against a minimal `shim/` of LVGL
(`lv_color_t` + `lv_color_hex`) and FreeRTOS (`SemaphoreHandle_t`); none of the
helpers under test touch LVGL widget globals or the shared `st`, so the module
links standalone.

These exist as the render-helper safety net identified by the Fowler audit
(`docs/exec-plans/active/fowler-audit-refactors.md` #3): `ui_render.c`'s layout
dispatchers reuse exactly these helpers, so this suite regression-guards them.

## Run

```
cd firmware/test/ui_format && make run
```

Exit code is non-zero if any assertion fails.
