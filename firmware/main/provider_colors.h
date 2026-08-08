// firmware/main/provider_colors.h
//
// Single source of truth for provider→brand-color mapping, shared by ui.c
// (LVGL accent) and led.c (hardware RGB LED). Mirrored verbatim from
// CodexBar's WidgetColors.color(for:) in CodexBarWidgetViews.swift. The `id`
// keys are the UsageProvider enum raw values (= the `provider` field in the
// Upstash payload).
//
// Local divergence: "opencode" uses the brand off-white #F1ECEC (not
// CodexBar's 0x3B82F6 blue) to match the two-tone "O" logo; "opencodego"
// intentionally keeps the blue. Do not "re-sync" these from upstream.
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct { const char *id; uint32_t hex; } prov_color_t;

static const prov_color_t PROV_COLORS[] = {
    { "codex",       0x49A3B0 }, { "openai",      0x0F826E },
    { "claude",      0xCC7C5E }, { "cursor",      0x00BFA5 },
    { "pi",          0xF2F2EE },
    { "opencode",    0xF1ECEC }, { "opencodego",  0xBDC053 },
    { "alibaba",     0xFF6A00 }, { "factory",     0xFF6B35 },
    { "gemini",      0xAB87EA }, { "antigravity", 0x60BA7E },
    { "copilot",     0xA855F7 }, { "zai",         0xE85A6A },
    { "minimax",     0xFE603C }, { "manus",       0x181818 },
    { "kimi",        0x23BDD9 }, { "kilo",        0xF27027 },
    { "kiro",        0xFF9900 }, { "vertexai",    0x4285F4 },
    { "augment",     0x6366F1 }, { "jetbrains",   0xFF3399 },
    { "kimik2",      0x4C00FF }, { "moonshot",    0x23BDD9 },
    { "amp",         0xDC2626 }, { "ollama",      0xEBEBE6 },
    { "synthetic",   0x141414 }, { "warp",        0x938BB4 },
    { "openrouter",  0xC8FF00 }, { "elevenlabs",  0xEBEBE6 },
    { "windsurf",    0x34E8BB }, { "perplexity",  0x20B2AA },
    { "mimo",        0xFF6900 }, { "doubao",      0x2D88FF },
    { "abacus",      0x38BDF8 },
    { "mistral",     0xFF500F },
    { "deepseek",    0x527DF0 }, { "codebuff",    0x44FF00 },
    { "crof",        0x2EAB94 }, { "venice",      0x3399FF },
    { "commandcode", 0x000000 }, { "stepfun",     0xFF8C00 },
    { "bedrock",     0xFF9900 }, { "grok",        0x10A37F },
    { "groq",        0xF56844 }, { "llmproxy",    0x24B47E },
    { "lmstudio",   0x7C3AED },
    { "deepgram",    0x0A121B },
    { "qwencloud",   0x665CEE },
};
#define PROV_COLORS_N  (sizeof(PROV_COLORS) / sizeof(*PROV_COLORS))

// Returns the brand-color hex for `id`, or 0 if not found.
static inline uint32_t prov_color_hex(const char *id)
{
    if (!id) return 0;
    for (unsigned i = 0; i < PROV_COLORS_N; i++)
        if (strcmp(id, PROV_COLORS[i].id) == 0)
            return PROV_COLORS[i].hex;
    return 0;
}
