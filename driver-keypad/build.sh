#!/usr/bin/env bash
# Package the NuVoxel Keypad Control4 driver into NuVoxelKeypad.c4z
# A .c4z is just a ZIP of the driver's CONTENTS (not a wrapping folder).
#
# The FILENAME is load-bearing. This driver declares a different <proxy> set from
# the retired MediaKeypad.c4z (it absorbs the companion intercom driver as a third
# proxy), and Control4 only instantiates proxies when a driver is first ADDED to a
# project — changing them under an installed driver corrupts it. So this must ship
# under a name that has never been installed. Never point OUT at MediaKeypad.c4z.
#
# The driver SOURCE (driver.lua, driver.xml) is stamp-free so it can be
# canonically synced with the public repo (see ../tools/sync-driver.sh). The
# build counter lives in version.txt (per-repo, not synced), and the real version
# is stamped only into a STAGED copy that gets zipped — never the tracked source.
set -euo pipefail

cd "$(dirname "$0")"
OUT="NuVoxelKeypad.c4z"
OUT_ABS="$PWD/$OUT"

# Required files sanity check
for f in driver.xml driver.lua json.lua; do
  [[ -f "$f" ]] || { echo "ERROR: missing $f" >&2; exit 1; }
done

# One date-based version drives BOTH the human-visible "Driver Version" string
# (YYYY.MM.DD.NNN + DRV, same scheme + tag as the firmware's …FW) AND Composer's
# <version> integer — its Sync-Local "is newer?" reload key (YYMMDDNNN: monotonic
# per date+seq, strictly increasing, within 32-bit). Counter persists in
# version.txt (absent → starts at today.001).
source "../tools/nvversion.sh"
CURDVER="$(cat version.txt 2>/dev/null || true)"
DVER="$(nv_next_version DRV "$CURDVER")"
IVER="$(nv_int_version "$DVER")"
[[ "$IVER" =~ ^[0-9]+$ ]] || { echo "ERROR: bad <version> int from '$DVER'" >&2; exit 1; }
printf '%s\n' "$DVER" > version.txt
NOW="$(date '+%m/%d/%Y %H:%M')"
echo "Driver Version = ${DVER}  (<version> = ${IVER})"

# Stage into a temp dir and stamp the STAGED copies only (the tracked driver.lua /
# driver.xml keep their placeholder version, so they stay byte-identical to the
# public repo for the canonical sync). Composer reads <version> from the .c4z.
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
cp driver.lua driver.xml json.lua "$STAGE/"
[[ -d icons ]] && cp -R icons "$STAGE/"
[[ -d www ]]   && cp -R www   "$STAGE/"
sed -i '' -E "s/DRIVER_VERSION *= *\"[^\"]*\"/DRIVER_VERSION = \"${DVER}\"/" "$STAGE/driver.lua"
sed -i '' -E "s#<version>[0-9]+</version>#<version>${IVER}</version>#"      "$STAGE/driver.xml"
sed -i '' -E "s#<modified>[^<]*</modified>#<modified>${NOW}</modified>#"     "$STAGE/driver.xml"
echo "Stamped <modified> = ${NOW} (staged copy)"

# Zip the staged contents (code + assets), excluding dev cruft and the archive.
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

# Drop a copy into Composer's local driver folder so "Update Driver" finds it
# without browsing. (Composer scans this dir; the project Add Driver search too.)
C4DIR="$HOME/Documents/Control4/Drivers"
if [[ -d "$C4DIR" ]]; then
  cp -f "$OUT" "$C4DIR/" && echo "Copied to $C4DIR/$OUT"
fi
echo "In Composer Pro: right-click the driver > Update Driver (or Add Driver to install fresh)."
