#!/usr/bin/env bash
# Build the UI sim (if needed) and render the canonical panel set to shots/.
# Each entry is "label WxH": the four ui.c layout "flavors" across the real
# panels, so one run shows how the shared UI reflows for every target.
set -euo pipefail
cd "$(dirname "$0")"

make -s

mkdir -p shots
BIN=build/mmk-sim

# label            W     H     (logical, post-rotation -- what ui.c sees)
panels=(
  "s3-landscape    320   240"   # lcdwiki 2.8\" rotated  -> flavor x4Ls
  "s3-portrait     240   320"   #                        -> flavor smallP
  "ws43-portrait   480   800"   # Waveshare 4.3\"        -> flavor x4P
  "nano-landscape  1280  800"   # P4 nano 10\"           -> flavor x4L
)

for p in "${panels[@]}"; do
  # shellcheck disable=SC2086
  set -- $p
  label=$1; w=$2; h=$3
  "$BIN" "$w" "$h" "shots/${label}.png"
done

echo "---"
echo "wrote $(ls shots/*.png | wc -l | tr -d ' ') PNGs to sim/shots/"
