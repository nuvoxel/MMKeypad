#!/usr/bin/env bash
# Regenerate main/mmk_ha_icons.c -- the Home Assistant theme's icon set, standing in
# for the real Control4 X4 icons (mmk_c4icons.c, partner-agreement licensed, not
# appropriate to show under a non-Control4 skin). Sourced from Material Design Icons
# (Pictogrammers, Apache-2.0) via the @mdi/svg npm package -- also what Home
# Assistant's own frontend uses, so it reads as authentically "HA" rather than a
# generic substitute. Requires node/npx (for the package fetch) + rsvg-convert
# (`brew install librsvg`) + the repo's Python (Pillow, for png2lvgl.py).
#
# Add/change an icon: add or edit a MAP entry below (internal_name:mdi-icon-name --
# browse names at pictogrammers.com/library/mdi), rerun, rebuild.
set -euo pipefail
cd "$(dirname "$0")"
OUT="../../main/mmk_ha_icons.c"
VER=7.4.47
SIZE=96   # rasterize resolution; iconBtnImg scales bitmaps freely, so one size covers
          # every on-screen slot (24-74px) the same way the C4 icon set does.

# internal_name -> MDI icon name. internal_name becomes `icon_ha_<internal_name>` in
# the generated C (matches the ICON_* macros in ui.c that pick this vs. the C4 icon).
MAP="media:music-note keypad:view-grid intercom:phone-in-talk room_add:home-plus
back:chevron-left chevron_down:chevron-down power:power
vol_up:volume-high vol_mute:volume-mute
thumb_up:thumb-up thumb_down:thumb-down
shuffle:shuffle-variant repeat:repeat dots:dots-vertical close:close
info:information group:account-group door:door-open
skip_prev:skip-previous skip_next:skip-next play:play pause:pause"

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
( cd "$TMP" && npm pack "@mdi/svg@$VER" >/dev/null 2>&1 && tar xzf mdi-svg-*.tgz )
SVGDIR="$TMP/package/svg"
[ -d "$SVGDIR" ] || { echo "ERROR: @mdi/svg not fetched (need network + npm)" >&2; exit 1; }

PNGDIR="$TMP/png"; mkdir -p "$PNGDIR"
MANIFEST="$TMP/ha.manifest"; : > "$MANIFEST"
for entry in $MAP; do
  internal="${entry%%:*}"; mdi="${entry##*:}"
  svg="$SVGDIR/$mdi.svg"
  [ -f "$svg" ] || { echo "ERROR: no MDI icon named '$mdi' (for $internal)" >&2; exit 1; }
  rsvg-convert -w "$SIZE" -h "$SIZE" -b none "$svg" -o "$PNGDIR/$internal.png"
  echo "ha_$internal  $PNGDIR/$internal.png" >> "$MANIFEST"
done

PNG2LVGL_HEADER="/* Generated from Material Design Icons (Pictogrammers, Apache-2.0)
 * via tools/icons/gen-ha-icons.sh -- A8 alpha masks tinted at render
 * time, matching mmk_c4icons.c's format. Do not edit by hand. */
" python3 png2lvgl.py "$OUT" "$MANIFEST"
echo "wrote $OUT"
