#!/usr/bin/env bash
# Package the NuVoxel Keypad Intercom Control4 driver into NuVoxelKeypadIntercom.c4z
#
# This is the SEPARATE intercom endpoint driver (companion to NuVoxelKeypad.c4z).
# It exists as its own driver — not a proxy inside the keypad driver — because the
# Communication agent V2 only enrolls a third-party intercom (driver_arch_type=5)
# when `intercomproxy` is the device's PRIMARY proxy. The merged keypad made
# keypad_proxy primary, so its intercom sub-proxy never enrolled (only "Everyone").
# This driver makes intercomproxy primary and
# reaches the device by relaying SIP/call traffic through the keypad driver's :6700
# link (control binding, class MMKEYPAD_INTERCOM) — the firmware :6700 server is
# single-client, so there is no second connection.
#
# The FILENAME is load-bearing (proxies are fixed at ADD time); ship only under a
# never-installed name. Source (driver.lua, driver.xml) stays stamp-free; the build
# counter lives in version.txt and the real version is stamped into a STAGED copy.
set -euo pipefail

cd "$(dirname "$0")"
OUT="NuVoxelKeypadIntercom.c4z"
OUT_ABS="$PWD/$OUT"

for f in driver.xml driver.lua json.lua intercom_proxy/intercom_command.lua; do
  [[ -f "$f" ]] || { echo "ERROR: missing $f" >&2; exit 1; }
done

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
cp -R intercom_proxy "$STAGE/"
[[ -d icons ]] && cp -R icons "$STAGE/"
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
      intercom_proxy/ \
      $([[ -d icons ]] && echo icons/) \
      -x '*.DS_Store' '*/.git/*' >/dev/null
)

echo "Built $OUT ($(du -h "$OUT" | cut -f1))"

C4DIR="$HOME/Documents/Control4/Drivers"
if [[ -d "$C4DIR" ]]; then
  cp -f "$OUT" "$C4DIR/" && echo "Copied to $C4DIR/$OUT"
fi
echo "The NuVoxel Agent installs + binds this driver per licensed device; a dealer never adds it by hand."
