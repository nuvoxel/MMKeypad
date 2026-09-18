#!/usr/bin/env bash
# Every SKU, every release -- checked, not just documented. A release missing
# one asset silently leaves that SKU with nothing to install (the on-screen
# picker just lists none), which already happened for real on v2026.09.06.001.
#
# Usage: tools/verify-release.sh <tag>     e.g. tools/verify-release.sh v2026.09.06.002
set -euo pipefail

tag="${1:?usage: verify-release.sh <tag>}"
repo="nuvoxel/MMKeypad"
ver="${tag#v}"

expected=(
  "mmk-poe-${ver}.bin"
  "mmk-nano-${ver}.bin"
  "mmk-ws43-${ver}.bin"
  "mmk-t3-${ver}.tar"
)

assets="$(gh release view "$tag" --repo "$repo" --json assets --jq '.assets[].name')"

missing=()
for name in "${expected[@]}"; do
  if ! grep -qxF "$name" <<<"$assets"; then
    missing+=("$name")
  fi
done

if [ "${#missing[@]}" -gt 0 ]; then
  echo "verify-release: $tag is missing ${#missing[@]} asset(s):" >&2
  for name in "${missing[@]}"; do
    echo "  - $name" >&2
  done
  exit 1
fi

echo "verify-release: $tag has all ${#expected[@]} expected assets"
