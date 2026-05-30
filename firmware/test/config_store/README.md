# config_store / wifi_lru host tests

Pure-C unit tests for the array-LRU logic extracted into `firmware/main/wifi_lru.h`.
No ESP-IDF or NVS headers required — only standard C11.

## Coverage
- Insert into empty list, sequential fill to `CFG_WIFI_MAX_ENTRIES` (append/prepend order)
- Duplicate SSID promotes to MRU without growing count; password overwritten
- Insert when full evicts LRU entry; new entry becomes MRU (count stays at max)
- Update of existing credential: overwrites password and promotes to MRU
- Count-never-exceeds-max boundary; single-entry and full-capacity boundaries
- `wifi_lru_promote` standalone: correct shift, password preserved, count unchanged
- `wifi_lru_find`: returns correct index and -1 for absent SSID
- Tail slots beyond `count` are zeroed after every upsert (no stale password residue)

## How to run
```sh
cd firmware/test/config_store
make        # compiles with cc -std=c11 and runs ./runtests
```
Expected output: `--- RESULTS: N passed, 0 failed ---` with exit code 0.
