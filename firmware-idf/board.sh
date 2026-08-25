#!/usr/bin/env bash
# Build/flash a board variant with the right MMK_BOARD + per-board sdkconfig.
# Each board uses its own build dir + sdkconfig so switching never clobbers another.
#
# Usage:  ./board.sh <s3|poe|nano|ws43|matrix> [idf.py args...]
# Examples:
#   ./board.sh poe build
#   ./board.sh s3  build
#   ./board.sh ws43 -p /dev/cu.wchusbserial410 flash monitor
set -euo pipefail
cd "$(dirname "$0")"

board="${1:-}"; shift || true
case "$board" in
  s3)  MMK=s3_lcdwiki; B=build;     TGT=esp32s3; DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32s3" ;;
  poe) MMK=p4_poe_eth; B=build.poe; TGT=esp32p4; DEFS="sdkconfig.defaults;sdkconfig.defaults.p4_poe_eth" ;;
  nano) MMK=p4_nano;   B=build.nano; TGT=esp32p4; DEFS="sdkconfig.defaults;sdkconfig.defaults.p4_nano" ;;
  ws43) MMK=ws43;      B=build.ws43; TGT=esp32p4; DEFS="sdkconfig.defaults;sdkconfig.defaults.ws43" ;;
  matrix) MMK=s3_matrix; B=build.matrix; TGT=esp32s3; DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32s3" ;;
  *) echo "usage: $0 <s3|poe|nano|ws43|matrix> [idf.py args...]"; exit 1 ;;
esac

# Release hardening: MMK_RELEASE=1 layers sdkconfig.release (logs/asserts/error
# strings stripped, build paths hidden — see that file) on top of the board
# defaults, in a separate *.rel build dir so it never clobbers the dev config.
# Set it when building a release image to publish (see OTA.md).
if [ -n "${MMK_RELEASE:-}" ]; then
  DEFS="${DEFS};sdkconfig.release"
  B="${B}.rel"
fi

# Some managed deps (esp_hosted) are gated per-BOARD in idf_component.yml
# via this env var — the component manager reads it while solving the manifest.
export MMK_BOARD="$MMK"

args=(-B "$B" -D SDKCONFIG="$B/sdkconfig" -D SDKCONFIG_DEFAULTS="$DEFS" -D MMK_BOARD="$MMK")

# A fresh build dir defaults to esp32, and both P4 boards share esp32p4 — so the
# component-manager target rules silently mis-resolve unless the target is set
# explicitly. Force set-target whenever the build dir's target doesn't match.
cur="$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"/\1/p' "$B/sdkconfig" 2>/dev/null || true)"
if [ "$cur" != "$TGT" ]; then
  idf.py "${args[@]}" set-target "$TGT"
fi

exec idf.py "${args[@]}" "$@"
