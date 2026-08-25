#!/usr/bin/env bash
# Package the NuVoxel Agent management driver into NuVoxelAgent.c4z
# A .c4z is just a ZIP of the driver's CONTENTS (not a wrapping folder).
#
# Mirrors driver/build.sh: the tracked source (driver.lua, driver.xml) is
# stamp-free, the build counter lives in this directory's own version.txt, and
# the real version is stamped only into a STAGED copy that gets zipped.
set -euo pipefail

cd "$(dirname "$0")"
OUT="NuVoxelAgent.c4z"
OUT_ABS="$PWD/$OUT"

for f in driver.xml driver.lua json.lua; do
  [[ -f "$f" ]] || { echo "ERROR: missing $f" >&2; exit 1; }
done

# One date-based version drives BOTH the visible "Driver Version" string
# (YYYY.MM.DD.NNN + DRV) AND Composer's <version> integer — its Sync-Local
# "is newer?" reload key (YYMMDDNNN). Counter persists in version.txt.
source "../tools/nvversion.sh"
CURDVER="$(cat version.txt 2>/dev/null || true)"
DVER="$(nv_next_version DRV "$CURDVER")"
IVER="$(nv_int_version "$DVER")"
[[ "$IVER" =~ ^[0-9]+$ ]] || { echo "ERROR: bad <version> int from '$DVER'" >&2; exit 1; }
printf '%s\n' "$DVER" > version.txt
NOW="$(date '+%m/%d/%Y %H:%M')"
echo "Driver Version = ${DVER}  (<version> = ${IVER})"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
cp driver.lua driver.xml json.lua "$STAGE/"
[[ -d icons ]] && cp -R icons "$STAGE/"
[[ -d www ]]   && cp -R www   "$STAGE/"
sed -i '' -E "s/DRIVER_VERSION *= *\"[^\"]*\"/DRIVER_VERSION = \"${DVER}\"/" "$STAGE/driver.lua"
sed -i '' -E "s#<version>[0-9]+</version>#<version>${IVER}</version>#"      "$STAGE/driver.xml"
sed -i '' -E "s#<modified>[^<]*</modified>#<modified>${NOW}</modified>#"     "$STAGE/driver.xml"
echo "Stamped <modified> = ${NOW} (staged copy)"

rm -f "$OUT"
(
  cd "$STAGE"
  zip -r -X "$OUT_ABS" \
      driver.xml \
      driver.lua \
      json.lua \
      $([[ -d icons ]] && echo icons/) \
      $([[ -d www ]] && echo www/) \
      -x '*.DS_Store' '*/.git/*' >/dev/null
)

echo "Built $OUT ($(du -h "$OUT" | cut -f1))"

# Drop a copy into Composer's local driver folder so the Agents list finds it
# without browsing.
C4DIR="$HOME/Documents/Control4/Drivers"
if [[ -d "$C4DIR" ]]; then
  cp -f "$OUT" "$C4DIR/" && echo "Copied to $C4DIR/$OUT"
fi
# This is an AGENT, and it used to be a room-placed device driver. Composer
# cannot re-type or re-parent an installed project item, so Update/Sync Local
# onto an old instance corrupts the project — always delete and re-add.
echo "In Composer Pro: Agents > Add > NuVoxel Agent. DELETE any existing instance first — never Update in place."
