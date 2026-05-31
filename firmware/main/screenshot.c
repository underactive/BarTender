// firmware/main/screenshot.c
//
// Listens for "screenshot\n" on stdin (the USB console serial port, shared
// with ESP_LOG). On match, forces a full LVGL re-render via ui_capture_screenshot(),
// then sends an SCAP-framed RGB565-LE image on stdout.
//
// Wire format sent to host:
//   "SCAP"          4 bytes  magic
//   width           uint16_t little-endian
//   height          uint16_t little-endian
//   pixel_bytes     uint32_t little-endian  (= width * height * 2)
//   pixel data      pixel_bytes bytes       RGB565-LE, row-major
//
// Host side: scripts/build/screenshot.py  (requires pyserial + Pillow)
#include "screenshot.h"
#include "display.h"
#include "ui.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "scap";

static void do_screenshot(void)
{
    // Enable the shadow copy only for this capture: ui_capture_screenshot()
    // forces a synchronous full re-render, so every tile flushes into the
    // shadow fb while enabled. Disable immediately after — the complete frame
    // now persists in the buffer for the reads below, and steady-state frames
    // go back to skipping the per-flush memcpy.
    display_shadow_capture(true);
    bool ok = ui_capture_screenshot();
    display_shadow_capture(false);
    if (!ok) {
        ESP_LOGW(TAG, "capture timed out");
        return;
    }
    int w = 0, h = 0;
    const uint8_t *fb = display_get_shadow_fb(&w, &h);
    if (!fb || w <= 0 || h <= 0) {
        ESP_LOGW(TAG, "shadow fb unavailable (PSRAM required)");
        return;
    }

    uint32_t plen = (uint32_t)w * (uint32_t)h * 2u;

    // Build the 12-byte SCAP header: magic(4) + w(2LE) + h(2LE) + length(4LE)
    uint8_t hdr[12];
    memcpy(hdr, "SCAP", 4);
    hdr[4]  = (uint8_t)(w       & 0xFF);
    hdr[5]  = (uint8_t)((w >>8) & 0xFF);
    hdr[6]  = (uint8_t)(h       & 0xFF);
    hdr[7]  = (uint8_t)((h >>8) & 0xFF);
    hdr[8]  = (uint8_t)( plen        & 0xFF);
    hdr[9]  = (uint8_t)((plen >>  8) & 0xFF);
    hdr[10] = (uint8_t)((plen >> 16) & 0xFF);
    hdr[11] = (uint8_t)((plen >> 24) & 0xFF);

    // ESP_LOG and our stdout both share the USB-serial TX path. Silence logging
    // for the duration of the binary send to prevent log bytes from corrupting
    // the pixel stream (the SCAP frame is binary, not line-delimited).
    esp_log_level_set("*", ESP_LOG_NONE);
    fwrite(hdr, 1, sizeof hdr, stdout);
    fwrite(fb,  1, (size_t)plen, stdout);
    fflush(stdout);
    esp_log_level_set("*", ESP_LOG_INFO);

    ESP_LOGI(TAG, "screenshot sent (%dx%d, %" PRIu32 " bytes)", w, h, plen);
}

static void screenshot_task(void *arg)
{
    (void)arg;
    char line[32];
    int pos = 0;
    for (;;) {
        int c = fgetc(stdin);
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r') continue;
        if (c == '\n') {
            line[pos] = '\0';
            if (strcmp(line, "screenshot") == 0) do_screenshot();
            pos = 0;
        } else if (pos < (int)sizeof line - 1) {
            line[pos++] = (char)c;
        } else {
            pos = 0;   // line too long: discard and reset
        }
    }
}

void screenshot_start(void)
{
    xTaskCreate(screenshot_task, "scap", 4096, NULL, 3, NULL);
}
