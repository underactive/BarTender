# Firmware Main Component

## Responsibility
Flat ESP-IDF application component for device runtime: boot orchestration, provisioning vs normal-mode gating, WiFi/fetch loop, UI rendering, touch input, LEDs, and screenshot/debug support.

## Dependencies
- **ESP-IDF / FreeRTOS**: task, queue, NVS, WiFi, HTTP, and board runtime patterns
- **LVGL**: single-task UI ownership and render-cycle constraints
- **Espressif display/touch/LED components**: hardware adapter shapes at the module edge

## Consumers
- **`app_main()`**: composes nearly every module in this layer
- **Peer modules in `firmware/main/`**: interact via typed headers, queues, and thread-safe setters
- **ESP-IDF build**: compiles this directory as the single `main` component

## Module Structure
```text
firmware/main/
├── main.c                                # Boot orchestration and mode gate
├── config_store.*, stats_model.*, app_event.h
│   # Persistence + typed payload/event contracts
├── net_wifi.*, upstash.*, fetch.*, provision.*
│   # Connectivity, remote fetch, captive-portal provisioning
├── ui.*, provider_*, font_*.c            # LVGL UI, branding registries, generated assets
├── display.*, touch.*, i2c_bus.*, led.*  # Hardware adapters around board config
└── screenshot.*, board_config.h, Kconfig.projbuild, idf_component.yml
   # Diagnostics + board capabilities + component config
```

## Boot Mode Gate (Provisioning Is Exclusive)
```c
void app_main(void) {
    init_nvs_or_recover();
    config_store_init();
    display_init();
    led_init();
    ui_start();                    // creates the sole LVGL-owning task

    bool have_cloud = config_store_has_remote();
    bool have_wifi = config_store_wifi_count() > 0;
    bool forced = config_store_take_setup_request(); // clear-before-act

    if (forced || !have_cloud || !have_wifi) {
        provision_start(/*wifi_only=*/have_cloud);
        return;                    // provisioning owns device until reboot
    }

    QueueHandle_t q = xQueueCreate(8, sizeof(app_evt_t));
    touch_init(q);
    net_wifi_start_multi();
    fetch_task_start(q);
}
```

## Single-Owner UI Task (All LVGL Calls Stay Here)
```c
void ui_set_stats(const stats_t *stats) {
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (stats) st.stats = *stats;  // copy-by-value; no borrowed pointers
    st.dirty = true;
    xSemaphoreGive(s_mtx);
}

static void ui_task(void *arg) {
    build_widgets_once();
    for (;;) {
        if (take_dirty_snapshot()) render(); // LVGL calls only on UI task
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
```

## Semantic Event Queue Boundary (Touch → UI → Optional App Action)
```c
// touch.c emits semantic gestures, not raw samples
app_evt_t ev = { .type = APP_EVT_TAP, .x = x, .y = y };
xQueueSend(app_q, &ev, 0);

// fetch/app loop forwards every event into UI first
if (xQueueReceive(app_q, &ev, timeout) == pdTRUE) {
    if (ui_handle_input(&ev) == UI_INPUT_PASS && ev.type == APP_EVT_LONG_PRESS) {
        request_setup_and_reboot(); // app-level side effect stays outside UI
    }
}
```

## Manager Task over Signal-Only Callbacks
```c
static void on_wifi_event(..., int32_t id, void *data) {
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        xTaskNotifyGive(s_mgr);    // callback only signals
    }
}

static void wifi_mgr_task(void *arg) {
    for (;;) {
        if (s_connected) { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); continue; }
        scan_known_networks();
        if (try_best_candidate()) config_store_wifi_promote(ssid);
        else vTaskDelay(backoff_ticks());
    }
}
```

## Summary balance tiles

`render_grid_tile()` shows OpenRouter, MiMo, Moonshot, DeepSeek, and Ramp balances as
`$X.XX` when their reduced payload balance is available. It reuses the existing
`row_bar[]` widget: `balance_bar_draw_cb()` handles `LV_EVENT_DRAW_POST` and
cuts $10 segments with screen-background divider lines. Every render resets the
shared bar's range and user data first, so a slot switching back to a percentage
tile cannot retain divider state.

## Architectural Boundaries
- **NO off-task LVGL calls**: other modules use `ui_set_*()` and `ui_handle_input()` only
- **NO raw NVS access outside `config_store`**: persistence schema and migrations live there
- **NO heavy work in ESP event callbacks**: callbacks signal; manager tasks own retries/scans
- **NO direct UI mutations from transport/parsing modules**: `upstash` fetches bytes, `stats_model` parses, `ui` renders

<important if="you are adding a new module to this layer">
## Adding a New Module
1. Keep the file in `firmware/main/` — this layer is a flat component first.
2. Place it under the right conceptual sublayer: persistence, connectivity, contracts, UI, or hardware adapter.
3. Expose a small `module_verb(...)` API in `foo.h` / `foo.c`.
4. Pull board-specific pins/capabilities from `board_config.h`, not inline constants.
5. If it owns async behavior, give it a dedicated task/queue/notification boundary.
6. Register new sources in `firmware/main/CMakeLists.txt` when needed.
</important>

<important if="you are adding or modifying persisted settings in this layer">
## Persisted Settings Checklist
1. Add typed getter/setter APIs in `config_store.h` / `config_store.c`.
2. Use independent NVS keys for unrelated scalars; use one blob when updates must stay atomic.
3. Validate loaded values before use and degrade safely on corruption.
4. Keep one-shot flags clear-before-act to avoid reboot/setup loops.
5. Wire consumers through `config_store_*` helpers only.
</important>

<important if="you are adding a new fetched field or provider-facing payload field to this layer">
## Payload Extension Checklist
1. Extend `stats_model.h` structs and `has_*` flags first.
2. Parse the new field in `stats_model.c`.
3. Publish it to the UI through `ui_set_stats()`.
4. Render it in `ui.c`; if provider-specific, update `provider_colors.h` / generated icons too.
5. Keep HTTP transport changes in `upstash.c`, not in the parser or UI.
</important>

<important if="you are adding a new gesture, input action, or navigation behavior to this layer">
## Input / Navigation Checklist
1. Add a new `APP_EVT_*` value in `app_event.h`.
2. Emit exactly one semantic event per gesture from `touch.c`.
3. Decide whether `ui_handle_input()` consumes or passes it.
4. Handle passed-through side effects in the app/fetch loop, not in `touch.c`.
5. Keep coordinates meaningful only for event types that need hit-testing.
</important>

<important if="you are adding a new hardware element to this layer">
## Hardware Adapter Checklist
1. Create a small `foo.h` / `foo.c` adapter with semantic operations.
2. Keep ESP-IDF/vendor setup inside that module only.
3. Reuse shared helpers like `i2c_bus_get()` for shared buses.
4. Read pins/capabilities from `board_config.h`.
5. Expose app-meaningful APIs, not raw driver internals.
</important>
