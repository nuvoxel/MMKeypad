#!/usr/bin/env bash
# Build the UI sim (if needed) and render the canonical panel set to shots/.
# Renders every page on every supported panel, so one run shows how the shared
# UI reflows -- and whether anything overflows -- before building firmware.
set -euo pipefail
cd "$(dirname "$0")"

make -s

mkdir -p shots
BIN=build/mmk-sim

# The supported targets. Layout follows ORIENTATION (ui.c: portrait = single
# column, landscape = two column) and s_uiscale carries the size, so these three
# cover every shape the UI has. The WS43 is the primary panel and its "Screen
# Rotation" driver property makes BOTH rows real, deployed configs -- a hero
# layout that overflowed 480px shipped unnoticed when only portrait was rendered.
# The T3 is landscape-only: bsp_linux.c rotates a portrait framebuffer itself.
# label            W     H     (logical, post-rotation -- what ui.c sees)
panels=(
  "ws43-portrait   480   800"   # Waveshare 4.3"
  "ws43-landscape  800   480"   # Waveshare 4.3" rotated
  "t3-landscape    1280  800"   # T3 7"/10" (and the P4 nano 10.1")
)

# Every page, not just home: each entry is "suffix ENV=... ENV=...".
pages=(
  "home"
  "nowplaying MMK_NOWPLAYING=1"
  "rooms      MMK_ROOMS=1"
  "intercom   MMK_IC=1"
  "comfort    MMK_COMFORT_PANEL=list"
  "thermostat MMK_COMFORT_PANEL=detail"
  "security   MMK_SEC_PANEL=1"
  "pin        MMK_SEC_PANEL=1 MMK_PIN=1"
  "settings   MMK_SETTINGS=1"
  "network    MMK_SETTINGS=1 MMK_SETPAGE=3"
  "call       MMK_CALL=incoming"
  "setup      MMK_SETUP=1"
)

for p in "${panels[@]}"; do
  # shellcheck disable=SC2086
  set -- $p
  label=$1; w=$2; h=$3
  for pg in "${pages[@]}"; do
    # shellcheck disable=SC2086
    set -- $pg
    name=$1; shift
    env "$@" "$BIN" "$w" "$h" "shots/${label}-${name}.png" >/dev/null
  done
done

echo "---"
echo "wrote $(ls shots/*.png | wc -l | tr -d ' ') PNGs to sim/shots/"
