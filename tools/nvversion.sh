#!/usr/bin/env bash
# Shared date-based version scheme for the MMKeypad driver + firmware, so both
# halves are versioned the same way (only the trailing type tag differs).
#
#   Format: YYYY.MM.DD.NNN<TYPE>     e.g. 2026.06.03.001FW  /  2026.07.15.002DRV
#   NNN is a per-day build sequence that resets to 001 each new day.
#
# Usage (source, then call):
#   source tools/nvversion.sh
#   nv_next_version FW  "$(cat version.txt)"     # -> 2026.07.15.001FW
#   nv_next_version DRV "$current_driver_version"
nv_next_version() {
  local type="$1" cur="${2:-}" today curdate curseq seq
  today="$(date '+%Y.%m.%d')"
  curdate="$(printf '%s' "$cur" | grep -oE '^[0-9]{4}\.[0-9]{2}\.[0-9]{2}' || true)"
  curseq="$(printf '%s' "$cur" | grep -oE '\.[0-9]{3}(FW|DRV)$' | grep -oE '[0-9]{3}' || true)"
  if [ "$curdate" = "$today" ] && [ -n "$curseq" ]; then
    seq="$(printf '%03d' "$((10#$curseq + 1))")"
  else
    seq="001"
  fi
  printf '%s.%s%s\n' "$today" "$seq" "$type"
}

# Integer form of a version string for Control4's <version> field (Composer's
# Sync-Local "is newer?" key). YYMMDDNNN — drops the century + type tag so it's
# monotonic and stays within 32-bit. 2026.07.15.003DRV -> 260715003
nv_int_version() {
  printf '%s' "${1:-}" \
    | sed -E 's/^[0-9]{2}([0-9]{2})\.([0-9]{2})\.([0-9]{2})\.([0-9]{3})(FW|DRV)$/\1\2\3\4/'
}
