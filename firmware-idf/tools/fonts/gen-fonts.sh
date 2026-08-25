#!/usr/bin/env bash
# Regenerate the MMKeypad LVGL fonts (output: main/mmk_text_*.c, later mmk_icons_*.c).
#
# Type face = Roboto (Control4 / Navigator typeface, Apache-2.0). Symbols (♪ ★ ♥ ™
# arrows ✓ ✗ ′ ″) merged from DejaVu Sans so keypad/now-playing glyphs render.
#
# Sources (not committed — fetch once):
#   Roboto-Regular.ttf / Roboto-Medium.ttf : Google Fonts (Apache-2.0). Also shipped in
#     the macOS X4 app: /Applications/Control4.app/Wrapper/X4.app/C4UIKit_C4UIKit.bundle/
#   DejaVuSans.ttf : Bitstream Vera derivative (permissive).
#
# Requires: node + `npx lv_font_conv`.
set -euo pipefail
cd "$(dirname "$0")"
OUT="../../main"

LAT="-r 0x20-0x7F -r 0xA0-0x17F -r 0x2013-0x2014 -r 0x2018-0x2019 -r 0x201C-0x201D -r 0x2022 -r 0x2026"
SYM="-r 0x2122 -r 0x2190-0x2193 -r 0x2605-0x2606 -r 0x2660-0x2667 -r 0x2669-0x266C -r 0x2713 -r 0x2717 -r 0x2032-0x2033"

gen () { # size  ttf  name
  npx --yes lv_font_conv@1.5.3 --size "$1" --bpp 4 --format lvgl --no-compress \
    --font "$2" $LAT --font ./DejaVuSans.ttf $SYM \
    -o "$OUT/$3.c" --lv-font-name "$3"
  echo "built $OUT/$3.c"
}

gen 14 ./Roboto-Regular.ttf mmk_text_14
gen 16 ./Roboto-Regular.ttf mmk_text_16
gen 24 ./Roboto-Medium.ttf  mmk_text_24
gen 32 ./Roboto-Medium.ttf  mmk_text_32
echo "done. (Phase 2: add Lucide icon font -> mmk_icons_*.c)"
