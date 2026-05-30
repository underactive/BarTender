// Host shim: replaces ESP-IDF esp_log.h for host-compiled unit tests.
// Only the macros/symbols actually referenced by stats_model.c are defined.
#pragma once

// No-op logging macros — stats_model.c uses ESP_LOGI only.
#define ESP_LOGE(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)
