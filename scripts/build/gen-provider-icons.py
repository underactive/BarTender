#!/usr/bin/env python3
# gen-provider-icons.py — rasterize the vendored CodexBar provider logo SVGs
# (scripts/build/assets/codexbar-logos/, sourced from github.com/steipete/CodexBar
# docs/logos/) into small LVGL images and emit
# firmware/main/provider_icons.{c,h}.
#
# Most SVGs become **A8** (one alpha byte per pixel, shape coverage only, no
# color). The firmware tints these via the LVGL image_recolor style.
# SVGs listed in FULL_COLOR_SVGS are rendered as **ARGB8888** (full RGBA
# with the SVG's own colors) and the firmware skips recolor for those.
#
# Deps: rsvg-convert (brew install librsvg) + Pillow. Re-run after changing
# the vendored SVGs or ICON_PX; the generated .c is committed (build input,
# like the LVGL fonts).
#
#   python3 scripts/build/gen-provider-icons.py
import os, subprocess, sys, tempfile
try:
    from PIL import Image, ImageFilter
except ImportError:
    sys.exit("Pillow not installed — pip install Pillow")

ICON_PX = 32
SUMMARY_ICON_PX = 32
BG_PX = 128
HERE = os.path.dirname(os.path.abspath(__file__))
SVG_DIR = os.path.join(HERE, "assets", "codexbar-logos")
OUT_C = os.path.join(HERE, "..", "..", "firmware", "main", "provider_icons.c")
OUT_H = os.path.join(HERE, "..", "..", "firmware", "main", "provider_icons.h")

# SVG basenames whose native colors should be preserved (ARGB8888).
# Everything else is rendered as A8 (silhouette, tinted by firmware).
FULL_COLOR_SVGS = {"claude", "lmstudio", "ollama"}

# Wide logos with heavy transparent padding: crop to opaque bounds, then
# scale to fill ICON_PX. Optional boost >1.0 nudges wide marks to match
# square silhouettes (lmstudio.png has ~35% vertical slack at 1.0).
CONTENT_FIT_SVGS = {
    "lmstudio", "claude", "codex", "openrouter", "gemini", "cursor",
    "pi", "copilot", "ollama", "opencode", "opencodego", "deepseek",
    "alibaba", "amp", "antigravity", "augment", "elevenlabs",
    "factory", "jetbrains", "kilo", "kimi", "kimik2", "kiro",
    "minimax", "mistral", "perplexity", "vertexai", "synthetic",
    "warp", "codebuff",
}
CONTENT_FIT_BOOST = {
    "lmstudio": 0.95,
    "pi": 0.80,
    "claude": 0.94,
    "codex": 0.80,
    "openrouter": 0.92,
    "gemini": 0.90,
    "cursor": 0.80,
}

# provider id (UsageProvider raw value == payload `id`) -> svg basename.
# Several ids legitimately share one logo.
ID_TO_SVG = {
    "claude": "claude", "codex": "codex", "openai": "codex", "pi": "pi",
    "cursor": "cursor", "openrouter": "openrouter",
    "opencode": "opencode", "opencodego": "opencode",
    "ollama": "ollama", "gemini": "gemini", "copilot": "copilot",
    "alibaba": "alibaba", "amp": "amp", "antigravity": "antigravity",
    "augment": "augment", "deepseek": "deepseek", "elevenlabs": "elevenlabs",
    "factory": "droid", "jetbrains": "jetbrains-ai", "kilo": "kilo",
    "kimi": "kimi", "kimik2": "kimi-k2", "kiro": "kiro",
    "minimax": "minimax", "mistral": "mistral", "perplexity": "perplexity",
    "vertexai": "vertex-ai", "abacus": "abacus-ai-dark", "zai": "zai-dark",
    "synthetic": "synthetic", "warp": "warp", "codebuff": "codebuff",
    "lmstudio": "lmstudio",
}


def _load_rgba(path):
    if path.lower().endswith(".svg"):
        with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
            png = tmp.name
        try:
            try:
                subprocess.run(
                    ["rsvg-convert", "-w", str(ICON_PX * 4),
                     "--background-color", "none", path, "-o", png],
                    check=True, capture_output=True)
            except FileNotFoundError:
                sys.exit("rsvg-convert not found — install with: brew install librsvg")
            return Image.open(png).convert("RGBA")
        finally:
            os.unlink(png)
    return Image.open(path).convert("RGBA")


def rasterize(path, px, base=None):
    """SVG or PNG -> px square RGBA, aspect-preserved, centered, transparent."""
    im = _load_rgba(path)
    boost = CONTENT_FIT_BOOST.get(base, 1.0) if base else 1.0
    if base in CONTENT_FIT_SVGS:
        bb = im.split()[-1].getbbox()
        if bb:
            im = im.crop(bb)
            cw, ch = im.size
            target = px * boost
            scale = target / max(cw, ch)
            nw = max(1, int(cw * scale))
            nh = max(1, int(ch * scale))
            im = im.resize((nw, nh), Image.LANCZOS)
        else:
            im.thumbnail((px, px), Image.LANCZOS)
    else:
        im.thumbnail((px, px), Image.LANCZOS)
    canvas = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    canvas.paste(im, ((px - im.width) // 2,
                      (px - im.height) // 2))
    return canvas


def rgba_to_argb8888(rgba):
    """RGBA PIL Image -> flat list of ARGB8888 bytes (B, G, R, A per LVGL).

    LVGL's lv_color32_t is BGRA in memory on little-endian (ESP32).
    Returns one contiguous list of bytes for the whole ICON_PX^2 image.
    """
    pixels = list(rgba.getdata())
    out = []
    for r, g, b, a in pixels:
        out.extend([b, g, r, a])  # LVGL lv_color32_t: blue, green, red, alpha
    return out


def main():
    if not os.path.isdir(SVG_DIR):
        sys.exit(f"missing {SVG_DIR}")
    # Generate one map per unique svg; ids reference them.
    svgs = sorted(set(ID_TO_SVG.values()))
    maps = {}        # svg basename -> tuple(list[int], str)
                      #   (data bytes, color_format_string)
    for base in svgs:
        # Support both .svg and .png source assets
        svg_path = os.path.join(SVG_DIR, base + ".svg")
        png_path = os.path.join(SVG_DIR, base + ".png")
        path = svg_path if os.path.isfile(svg_path) else png_path
        if not os.path.isfile(path):
            print(f"skip {base}: no svg", file=sys.stderr)
            continue
        rgba = rasterize(path, ICON_PX, base)
        rgba_summary = rasterize(path, SUMMARY_ICON_PX, base)
        rgba_bg = rasterize(path, BG_PX, base)
        bg = rgba_bg.filter(ImageFilter.GaussianBlur(3))
        alpha = bg.getchannel("A").point(lambda a: int(a * 0.50))
        bg.putalpha(alpha)
        if base in FULL_COLOR_SVGS:
            data = rgba_to_argb8888(rgba)
            data_summary = rgba_to_argb8888(rgba_summary)
            data_bg = rgba_to_argb8888(bg)
            cf = "LV_COLOR_FORMAT_ARGB8888"
            cf_bg = "LV_COLOR_FORMAT_ARGB8888"
        else:
            alpha = list(rgba.split()[-1].tobytes())  # row-major, 1 byte/px
            alpha_summary = list(rgba_summary.split()[-1].tobytes())
            alpha_bg = list(bg.split()[-1].tobytes())
            if max(alpha) < 8 or max(alpha_summary) < 8 or max(alpha_bg) < 8:
                print(f"skip {base}: rasterized blank", file=sys.stderr)
                continue
            data = alpha
            data_summary = alpha_summary
            data_bg = alpha_bg
            cf = "LV_COLOR_FORMAT_A8"
            cf_bg = "LV_COLOR_FORMAT_A8"
        maps[base] = (data, data_summary, data_bg, cf, cf_bg)

    sym = lambda b: "ic_" + b.replace("-", "_")
    n_a8 = ICON_PX * ICON_PX
    n_argb = n_a8 * 4
    n_sum_a8 = SUMMARY_ICON_PX * SUMMARY_ICON_PX
    n_sum_argb = n_sum_a8 * 4
    n_bg_a8 = BG_PX * BG_PX
    n_bg_argb = n_bg_a8 * 4
    c = ['// AUTO-GENERATED by scripts/build/gen-provider-icons.py — do not edit.',
         '// Source SVGs: scripts/build/assets/codexbar-logos/ (from CodexBar repo).',
         '#include "provider_icons.h"', '#include "lvgl.h"', '']
    for base in sorted(maps):
        data, data_summary, data_bg, cf, cf_bg = maps[base]
        is_full = base in FULL_COLOR_SVGS
        n = n_argb if is_full else n_a8
        stride = ICON_PX * 4 if is_full else ICON_PX
        n_sum = n_sum_argb if is_full else n_sum_a8
        stride_sum = SUMMARY_ICON_PX * 4 if is_full else SUMMARY_ICON_PX
        n_bg = n_bg_argb if is_full else n_bg_a8
        stride_bg = BG_PX * 4 if is_full else BG_PX
        rows = []
        for r in range(ICON_PX):
            row = data[r * stride:(r + 1) * stride]
            rows.append("    " + ",".join(str(v) for v in row) + ",")
        rows_sum = []
        for r in range(SUMMARY_ICON_PX):
            row = data_summary[r * stride_sum:(r + 1) * stride_sum]
            rows_sum.append("    " + ",".join(str(v) for v in row) + ",")
        c.append(f"static const uint8_t {sym(base)}_map[{n}] = {{")
        c.extend(rows)
        c.append("};")
        c.append(f"static const lv_image_dsc_t {sym(base)} = {{")
        c.append("    .header = { .magic = LV_IMAGE_HEADER_MAGIC,")
        c.append(f"                .cf = {cf},")
        c.append(f"                .w = {ICON_PX}, .h = {ICON_PX},"
                 f" .stride = {stride} }},")
        c.append(f"    .data_size = {n}, .data = {sym(base)}_map,")
        c.append("};")
        c.append("")
        c.append(f"static const uint8_t {sym(base)}_summary_map[{n_sum}] = {{")
        c.extend(rows_sum)
        c.append("};")
        c.append(f"static const lv_image_dsc_t {sym(base)}_summary = {{")
        c.append("    .header = { .magic = LV_IMAGE_HEADER_MAGIC,")
        c.append(f"                .cf = {cf},")
        c.append(f"                .w = {SUMMARY_ICON_PX}, .h = {SUMMARY_ICON_PX},"
                 f" .stride = {stride_sum} }},")
        c.append(f"    .data_size = {n_sum}, .data = {sym(base)}_summary_map,")
        c.append("};")
        c.append("")
        c.append(f"static const uint8_t {sym(base)}_bg_map[{n_bg}] = {{")
        rows_bg=[]
        for r in range(BG_PX):
            row = data_bg[r*stride_bg:(r+1)*stride_bg]
            rows_bg.append("    " + ",".join(str(v) for v in row) + ",")
        c.extend(rows_bg)
        c.append("};")
        c.append(f"static const lv_image_dsc_t {sym(base)}_bg = {{")
        c.append("    .header = { .magic = LV_IMAGE_HEADER_MAGIC,")
        c.append(f"                .cf = {cf_bg},")
        c.append(f"                .w = {BG_PX}, .h = {BG_PX},"
                 f" .stride = {stride_bg} }},")
        c.append(f"    .data_size = {n_bg}, .data = {sym(base)}_bg_map,")
        c.append("};")
        c.append("")
    c.append("const lv_image_dsc_t *provider_icon(const char *id)")
    c.append("{")
    c.append("    if (!id) return NULL;")
    c.append("    static const struct { const char *id;"
             " const lv_image_dsc_t *ic; } M[] = {")
    for pid in sorted(ID_TO_SVG):
        base = ID_TO_SVG[pid]
        if base in maps:
            c.append(f'        {{ "{pid}", &{sym(base)} }},')
    c.append("    };")
    c.append("    for (unsigned i = 0; i < sizeof M / sizeof *M; i++)")
    c.append("        if (strcmp(id, M[i].id) == 0) return M[i].ic;")
    c.append("    return NULL;")
    c.append("}")
    c.append("")
    c.append("const lv_image_dsc_t *provider_summary_icon(const char *id)")
    c.append("{")
    c.append("    if (!id) return NULL;")
    c.append("    static const struct { const char *id; const lv_image_dsc_t *ic; } M[] = {")
    for pid in sorted(ID_TO_SVG):
        base = ID_TO_SVG[pid]
        if base in maps:
            c.append(f'        {{ "{pid}", &{sym(base)}_summary }},')
    c.append("    };")
    c.append("    for (unsigned i = 0; i < sizeof M / sizeof *M; i++)")
    c.append("        if (strcmp(id, M[i].id) == 0) return M[i].ic;")
    c.append("    return NULL;")
    c.append("}")
    c.append("")
    c.append("const lv_image_dsc_t *provider_background_icon(const char *id)")
    c.append("{")
    c.append("    if (!id) return NULL;")
    c.append("    static const struct { const char *id; const lv_image_dsc_t *ic; } M[] = {")
    for pid in sorted(ID_TO_SVG):
        base = ID_TO_SVG[pid]
        if base in maps:
            c.append(f'        {{ "{pid}", &{sym(base)}_bg }},')
    c.append("    };")
    c.append("    for (unsigned i = 0; i < sizeof M / sizeof *M; i++)")
    c.append("        if (strcmp(id, M[i].id) == 0) return M[i].ic;")
    c.append("    return NULL;")
    c.append("}")
    c.append("")
    c.append("bool provider_icon_is_full_color(const char *id)")
    c.append("{")
    c.append("    if (!id) return false;")
    c.append("    static const char *F[] = {")
    for pid in sorted(ID_TO_SVG):
        base = ID_TO_SVG[pid]
        if base in FULL_COLOR_SVGS and base in maps:
            c.append(f'        "{pid}",')
    c.append("    };")
    c.append("    for (unsigned i = 0; i < sizeof F / sizeof *F; i++)")
    c.append("        if (strcmp(id, F[i]) == 0) return true;")
    c.append("    return false;")
    c.append("}")
    c.append("")
    with open(OUT_C, "w") as f:
        f.write("\n".join(c))
    with open(OUT_H, "w") as f:
        f.write("// AUTO-GENERATED by scripts/build/gen-provider-icons.py.\n"
                "#pragma once\n#include \"lvgl.h\"\n#include <stdbool.h>\n#include <string.h>\n\n"
                "// A8 silhouette or ARGB8888 full-color image, or NULL.\n"
                "// A8 icons must be tinted via lv_image's image_recolor style;\n"
                "// ARGB8888 icons carry their own colors (check is_full_color).\n"
                "const lv_image_dsc_t *provider_icon(const char *id);\n"
                "\n"
                "// Compact icon variant for the summary provider rows.\n"
                "const lv_image_dsc_t *provider_summary_icon(const char *id);\n"
                "\n"
                "// Background watermark variant (large, blurred) for per-provider pages.\n"
                "const lv_image_dsc_t *provider_background_icon(const char *id);\n"
                "\n"
                "// Returns true if the icon for `id` is ARGB8888 (full color).\n"
                "// When true, do NOT apply image_recolor to the icon.\n"
                "bool provider_icon_is_full_color(const char *id);\n")
    print(f"wrote {len(maps)} icons -> {os.path.relpath(OUT_C, HERE+'/..')}")


if __name__ == "__main__":
    main()
