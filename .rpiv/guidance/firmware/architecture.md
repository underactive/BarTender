# Firmware Package

## Responsibility
ESP-IDF project root for the desk-toy firmware. It defines the build, target configuration, partitions, managed component dependencies, and the single first-party application component under `firmware/main/`.

## Dependencies
- **ESP-IDF / CMake**: project structure, component registration, build/flash workflow
- **Managed components (`idf_component.yml`, `dependencies.lock`)**: LVGL, panel, touch, and LED packages fetched into the build

## Consumers
- **ESP-IDF tooling**: consumes `CMakeLists.txt`, `sdkconfig.defaults*`, `partitions.csv`, and `main/`
- **`firmware/main/`**: first-party runtime code compiled within this package
- **Repo docs / humans**: `firmware/README.md` is the operator guide for build, flash, and provisioning

## Module Structure
```text
firmware/
├── main/                     # First-party runtime component (see guidance file below)
├── CMakeLists.txt            # Project root for ESP-IDF
├── sdkconfig.defaults*       # Checked-in target/runtime defaults
├── partitions.csv            # Flash layout contract
├── dependencies.lock
│   idf_component.yml         # Managed component dependency surface
├── README.md                 # Build/flash/provisioning workflow
└── build/, managed_components/
   # Generated output and fetched deps; not authored architecture
```

## Single Component Registration (First-Party Code Lives in `main/`)
```cmake
idf_component_register(
  SRCS
    "main.c" "config_store.c" "display.c" "fetch.c" "i2c_bus.c"
    "led.c" "net_wifi.c" "provision.c" "screenshot.c"
    "stats_model.c" "touch.c" "ui.c" "upstash.c"
  INCLUDE_DIRS "."
  REQUIRES freertos nvs_flash esp_wifi esp_http_client esp_http_server
           esp_lcd led_strip json lwip esp_event esp_netif
)
```

## Managed Component Boundary (External Packages Stay Outside First-Party Modules)
```yaml
dependencies:
  idf: '>=5.3'
  lvgl/lvgl: ^9
  espressif/esp_lcd_ili9341: ^2
  espressif/esp_lcd_touch_ft5x06: ^1
  espressif/led_strip: ^3
```

## Architectural Boundaries
- **NO first-party logic in `build/` or `managed_components/`**: both are generated/fetched surfaces
- **NO board or runtime defaults hidden only in local `sdkconfig`**: checked-in defaults belong in `sdkconfig.defaults*`
- **NO duplicate architecture notes for runtime internals here**: `firmware/main/architecture.md` is the authoritative module-level guide

<important if="you are changing firmware runtime behavior or adding first-party firmware code">
- Put the implementation in `firmware/main/`; see `.rpiv/guidance/firmware/main/architecture.md` for runtime patterns.
- Update `firmware/main/CMakeLists.txt` when adding sources.
- Keep build/config changes (`sdkconfig.defaults*`, `partitions.csv`, component deps) in `firmware/`, not scattered into app modules.
</important>

<important if="you are adding or changing managed firmware dependencies">
- Declare them in `firmware/main/idf_component.yml` and let `dependencies.lock` capture the resolved versions.
- Treat `managed_components/` as fetched output; do not hand-edit vendored code there.
- If a dependency changes architecture constraints (display stack, UI, networking), reflect that in the relevant guidance file.
</important>
