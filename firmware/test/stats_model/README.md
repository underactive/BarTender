# stats_model host unit tests

Compiles and exercises the **real** `firmware/main/stats_model.c` (not a copy) against a
vendored cJSON and a no-op `esp_log.h` shim so the pure-logic parsing code can be tested
with host `cc` without any ESP32 toolchain.

Covers: v1/v2 parse happy paths, null/absent/non-string result handling, malformed JSON,
unknown schema version rejection, `ok:false` entries, `i32_clamp`/`i64_clamp` saturation,
`pct_hist` 0-100 clamping, provider/hist/pct-hist/lm-models/lm-week/cu-hist array caps,
pi/cursor/lmstudio provider-specific blocks, `stats_model_reorder` ordering and edge cases.

To build and run:

```sh
cd firmware/test/stats_model && make
```
