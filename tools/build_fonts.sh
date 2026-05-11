#!/usr/bin/env bash
# Generate IBM Plex Sans + FontAwesome symbol glyphs as LVGL C arrays.
# Requires lv_font_conv installed globally (npm i -g lv_font_conv).
set -euo pipefail

cd "$(dirname "$0")/.."
TOOLS=tools/fonts
OUT=src/fonts

# FontAwesome symbol range used by LVGL widgets (lv_symbol_def.h)
SYMBOLS="0xF001,0xF008,0xF00B,0xF00C,0xF00D,0xF011,0xF013,0xF015,0xF019,0xF01C,0xF021,0xF026,0xF027,0xF028,0xF03E,0xF048,0xF04B,0xF04C,0xF04D,0xF051,0xF052,0xF053,0xF054,0xF067,0xF068,0xF06E,0xF070,0xF071,0xF074,0xF077,0xF078,0xF079,0xF07B,0xF093,0xF095,0xF0C4,0xF0C5,0xF0C7,0xF0E7,0xF0EA,0xF0F3,0xF11C,0xF124,0xF158,0xF1EB,0xF240,0xF241,0xF242,0xF243,0xF244,0xF287,0xF293,0xF2ED,0xF304,0xF55A,0xF7C2,0xF8A2"

gen() {
    local face="$1" weight="$2" size="$3"
    local out="$OUT/plex_${weight}_${size}.c"
    echo "  -> $out"
    lv_font_conv \
        --bpp 4 --size "$size" \
        --font "$TOOLS/${face}.ttf" -r 0x20-0x7F \
        --font "$TOOLS/symbols.woff" -r "$SYMBOLS" \
        --format lvgl --no-compress \
        -o "$out"
}

mkdir -p "$OUT"
# Use BOLD for every size — the panel is low-contrast so thin/medium weights
# wash out. Keeping the "plex_medium_*" filenames keeps ui.cpp unchanged.
for size in 12 14 16 18; do
    gen "ibmplexsans-bold" "medium" "$size"
done
gen "ibmplexsans-bold" "medium" 22
gen "ibmplexsans-bold" "medium" 28
gen "ibmplexsans-bold" "bold" 22
gen "ibmplexsans-bold" "bold" 28
gen "ibmplexsans-bold" "bold" 48

cat > "$OUT/fonts.h" <<'EOF'
#pragma once
#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include <lvgl.h>
#endif

LV_FONT_DECLARE(plex_medium_12);
LV_FONT_DECLARE(plex_medium_14);
LV_FONT_DECLARE(plex_medium_16);
LV_FONT_DECLARE(plex_medium_18);
LV_FONT_DECLARE(plex_medium_22);
LV_FONT_DECLARE(plex_medium_28);
LV_FONT_DECLARE(plex_bold_22);
LV_FONT_DECLARE(plex_bold_28);
LV_FONT_DECLARE(plex_bold_48);
EOF

echo "done."
