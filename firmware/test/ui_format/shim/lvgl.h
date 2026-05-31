// Minimal host shim of lvgl.h — ONLY the surface that ui_format.c and
// ui_internal.h reference. ui_format.c uses lv_color_t + lv_color_hex; the rest
// (lv_obj_t, lv_font_t, lv_chart_series_t, lv_opa_t) appear only in struct/extern
// declarations in ui_internal.h and never need real definitions for these tests.
#pragma once
#include <stdint.h>

// Real LVGL lv_color_t is a 3-byte RGB struct; the field layout is irrelevant
// here as long as lv_color_hex round-trips a 0xRRGGBB value the tests can check.
typedef struct { uint8_t blue, green, red; } lv_color_t;

static inline lv_color_t lv_color_hex(uint32_t c)
{
    lv_color_t r;
    r.red   = (uint8_t)((c >> 16) & 0xff);
    r.green = (uint8_t)((c >>  8) & 0xff);
    r.blue  = (uint8_t)( c        & 0xff);
    return r;
}

typedef uint8_t lv_opa_t;
#define LV_OPA_90 ((lv_opa_t)230)

// Opaque widget/font/series handles — only ever used as pointers in struct defs.
typedef struct _lv_obj_t          lv_obj_t;
typedef struct _lv_font_t         lv_font_t;
typedef struct _lv_chart_series_t lv_chart_series_t;
