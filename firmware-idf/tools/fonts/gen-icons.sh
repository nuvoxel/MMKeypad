#!/usr/bin/env bash
# Regenerate the MMKeypad LVGL ICON fonts (main/mmk_icons_24.c + mmk_icons_34.c) from the
# Lucide icon font. Fetches lucide-static@1.22.0 — the version whose PUA codepoints match
# our glyphs (play = U+E13C, list = U+E106, …). Requires node + npx.
#
# Add an icon: find its codepoint in lucide-static's font/info.json (name -> encodedCode,
# e.g. phone = \e133), add it to CPS24 below, add a matching #define G_* in ui.c, rebuild.
#   24px (FICON)  = side / small / list-row icons.
#   34px (FICONL) = the large centre transport glyphs only (play/pause/stop).
set -euo pipefail
cd "$(dirname "$0")"
OUT="../../main"
VER=1.22.0

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
( cd "$TMP" && npm pack "lucide-static@$VER" >/dev/null 2>&1 && tar xzf lucide-static-*.tgz )
TTF="$TMP/package/font/lucide.ttf"
[ -f "$TTF" ] || { echo "ERROR: lucide.ttf not fetched (need network + npm)" >&2; exit 1; }

# bell speaker music mic play pause stop skip prev/next vol mute close info list dots
# thumbs shuffle repeat +  phone(E133) phone-call(E134) users(E1A4) door-open(E3D6)
CPS24="0xE059,0xE064,0xE06D,0xE070,0xE0B7,0xE0D2,0xE0F2,0xE0F5,0xE0F9,0xE106,0xE10B,0xE10C,0xE118,0xE11E,0xE122,0xE12E,0xE133,0xE134,0xE13C,0xE140,0xE146,0xE154,0xE158,0xE15E,0xE15F,0xE160,0xE165,0xE166,0xE167,0xE176,0xE178,0xE186,0xE189,0xE18A,0xE195,0xE1A4,0xE1A5,0xE1AB,0xE1AC,0xE1AE,0xE1B2,0xE1C2,0xE379,0xE37F,0xE3C0,0xE3D6,0xE3E6,0xE412,0xE55A"
CPS34="0xE13C,0xE12E,0xE167"

gen () { # size  cps  name
  npx --yes lv_font_conv@1.5.3 --size "$1" --bpp 4 --format lvgl --no-compress \
    --font "$TTF" -r "$2" -o "$OUT/$3.c" --lv-font-name "$3"
  echo "built $OUT/$3.c"
}
gen 24 "$CPS24" mmk_icons_24
gen 34 "$CPS34" mmk_icons_34
echo "done."
