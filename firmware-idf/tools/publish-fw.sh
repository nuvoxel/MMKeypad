#!/usr/bin/env bash
# Build, sign, hash, upload, and publish keypad firmware for one or more SKUs.
#
#   publish-fw.sh <version|auto> <channel> [sku ...]
#   e.g.  publish-fw.sh auto stable s3          # -> 2026.07.15.001FW (date scheme)
#         publish-fw.sh auto stable             # all SKUs, today's next seq
#         publish-fw.sh 2026.07.15.003FW beta   # explicit override
#
# Each image is signed with the Secure Boot v2 ECDSA key (FW_SIGN_KEY), uploaded
# to the public Blob container, and registered in the unified OTA catalog
# (platform.device_releases) via POST /api/v1/fw/publish — which is what
# /api/v1/fw/manifest + /fw/check now serve from. Secure Boot enforcement is
# deferred (eFuse); integrity on OTA is via sha256, authenticity via HTTPS + sig.
#
# Env (beyond the storage/signing keys below):
#   NUVOXEL_API_BASE  web app base url (default https://nuvoxel.com)
#   FW_PUBLISH_TOKEN  bearer token for the publish endpoint (required)
set -euo pipefail
cd "$(dirname "$0")/.."   # firmware-idf/
REPO_ROOT="$(cd ../../.. && pwd)"       # firmware-idf -> mmkeypad -> apps -> root
PUBLISH="$REPO_ROOT/scripts/publish-release.py"

# Every SHIPPED image is a release build: logs/asserts/error-strings stripped,
# build paths hidden (sdkconfig.release, via board.sh). Builds into *.rel dirs.
export MMK_RELEASE=1

VERSION="${1:?usage: publish-fw.sh <version|auto> <channel> [sku...]}"
CHANNEL="${2:?channel (e.g. stable|beta)}"
shift 2

# Date-based version scheme shared with the driver (YYYY.MM.DD.NNN + FW). Pass
# `auto` to advance to today's next sequence. We then write version.txt (the
# build's PROJECT_VER / fw_version) so the flashed binary reports EXACTLY the
# version we record in the manifest — otherwise the OTA equality check ("update
# if manifest != current") would never settle.
source "../tools/nvversion.sh"
if [ "$VERSION" = "auto" ]; then
  VERSION="$(nv_next_version FW "$(cat version.txt 2>/dev/null || true)")"
fi
printf '%s\n' "$VERSION" > version.txt
echo "firmware version: $VERSION (written to version.txt)"
SKUS=("$@"); [ ${#SKUS[@]} -eq 0 ] && SKUS=(s3 poe nano ws43 t3)

# Versioned format — bump when the shape changes; the consumer gates on it.
#   T3_BUNDLE_VERSION: the t3-bundle tar layout (nv_ota_t3.c must understand it),
#                      recorded as format_version on the release.
T3_BUNDLE_VERSION="${T3_BUNDLE_VERSION:-1}"

# The ESP SKUs are ESP Secure Boot v2 signed; the T3 (Linux) is sha256-only and
# needs no signing key. So only require FW_SIGN_KEY when a non-T3 SKU is present.
for s in "${SKUS[@]}"; do
  [ "$s" = "t3" ] || { : "${FW_SIGN_KEY:?set FW_SIGN_KEY to the Secure Boot v2 signing PEM}"; break; }
done
STORAGE_ACCOUNT="${FW_STORAGE_ACCOUNT:-hydrogenmediastorage}"
CONTAINER="${FW_CONTAINER:-firmware}"
STORAGE_KEY="${FW_STORAGE_KEY:-$(az storage account keys list --account-name "$STORAGE_ACCOUNT" --query '[0].value' -o tsv 2>/dev/null)}"
: "${STORAGE_KEY:?could not resolve a storage key; set FW_STORAGE_KEY}"
BLOB_BASE="https://${STORAGE_ACCOUNT}.blob.core.windows.net/${CONTAINER}"
: "${FW_PUBLISH_TOKEN:?set FW_PUBLISH_TOKEN (bearer for /api/v1/fw/publish)}"

# Portable (bash 3.2 / macOS) sku -> platform-id and sku -> build-dir maps.
sku_id()    { case "$1" in s3) echo mmk-s3;; poe) echo mmk-poe;; nano) echo mmk-nano;; ws43) echo mmk-ws43;; t3) echo mmk-t3;; *) return 1;; esac; }

# Promotion rule + blob helpers (blob_exists / blob_digest): a published
# (target, version) is FROZEN — re-running for a second channel registers the
# bytes already being served instead of rebuilding and re-signing over them.
# The full rationale lives in the lib. FW_FORCE_REPUBLISH=1 overrides.
source "$REPO_ROOT/scripts/blob-lib.sh"

# The T3 (RK3188 Linux) is a different animal from the ESP SKUs: a zig-built
# Linux app + a split-init overlay, no esptool/Secure-Boot, sha256-only OTA. Its
# artifact is a tar BUNDLE of both overlays that nv_ota_t3.c extracts + installs
# (/data/mmkeypad + /data/init.overlay) with per-overlay crash-loop rollback.
publish_t3() {
  local id="$1" T3="../firmware-linux-t3"

  # Same promotion rule as the ESP path: a published version's bytes are frozen.
  # The tar carries build-time file metadata, so a rebuild is not guaranteed to
  # reproduce byte-for-byte either — re-uploading would strand the sha256 the
  # other channel already published.
  local tblob="keypad/${id}/${VERSION}.tar"
  if blob_exists "$tblob" && [ -z "${FW_FORCE_REPUBLISH:-}" ]; then
    local tsha tsize
    read -r tsha tsize <<<"$(blob_digest "$tblob")"
    python3 "$PUBLISH" --kind linux-fw --target "$id" --channel "$CHANNEL" \
      --version "$VERSION" --url "${BLOB_BASE}/${tblob}" --sha256 "$tsha" --size-bytes "$tsize" \
      --format t3-bundle --format-version "$T3_BUNDLE_VERSION"
    echo "  promoted to ${CHANNEL} (existing bundle, $tsize bytes, sha256 ${tsha:0:12}...) — no rebuild"
    return 0
  fi

  echo "  building T3 app + init (zig cross-compile)"
  make -C "$T3/lvgl-app" >/dev/null
  make -C "$T3" init >/dev/null
  local app="$T3/lvgl-app/build/mmk-app" ini="$T3/build/init"
  [ -f "$app" ] && [ -f "$ini" ] || { echo "  T3 build missing ($app / $ini)"; exit 1; }
  local tmpd bundle; tmpd="$(mktemp -d)"; bundle="$(mktemp -t "${id}-${VERSION}").tar"
  cp "$app" "$tmpd/mmkeypad"; cp "$ini" "$tmpd/init.overlay"
  # Self-describing, VERSIONED bundle: meta.json (packed first so the consumer can
  # read it early) declares the format + which member is which. Bump "version"
  # when the layout changes; nv_ota_t3.c rejects a version it doesn't understand.
  printf '{"format":"t3-bundle","version":%s,"app":"mmkeypad","init":"init.overlay"}\n' \
     "$T3_BUNDLE_VERSION" > "$tmpd/meta.json"
  ( cd "$tmpd" && tar -cf "$bundle" meta.json mmkeypad init.overlay ); rm -rf "$tmpd"
  local sha size blob url
  sha=$(shasum -a 256 "$bundle" | awk '{print $1}')
  size=$(stat -f%z "$bundle" 2>/dev/null || stat -c%s "$bundle")
  blob="keypad/${id}/${VERSION}.tar"
  az storage blob upload --account-name "$STORAGE_ACCOUNT" --account-key "$STORAGE_KEY" \
     --container-name "$CONTAINER" --name "$blob" --file "$bundle" --overwrite -o none
  url="${BLOB_BASE}/${blob}"; rm -f "$bundle"
  # Linux/T3 firmware: a versioned tar bundle (app + init overlay), sha256-only.
  python3 "$PUBLISH" --kind linux-fw --target "$id" --channel "$CHANNEL" \
    --version "$VERSION" --url "$url" --sha256 "$sha" --size-bytes "$size" \
    --format t3-bundle --format-version "$T3_BUNDLE_VERSION"
  echo "  -> $url  ($size bytes, sha256 ${sha:0:12}...)  [t3-bundle v$T3_BUNDLE_VERSION: app + init]"
  # No esp-web-tools flasher: the T3 first-flash is rkdeveloptool/loader (see
  # firmware-linux-t3/README.md), not Web Serial.
}
build_dir() { case "$1" in s3) echo build;; poe) echo build.poe;; nano) echo build.nano;; ws43) echo build.ws43;; *) return 1;; esac; }
# esptool target + esp-web-tools chipFamily for the in-browser flasher.
idf_chip()  { case "$1" in s3) echo esp32s3;; poe|nano|ws43) echo esp32p4;; *) return 1;; esac; }
chip_family(){ case "$1" in s3) echo "ESP32-S3";; poe|nano|ws43) echo "ESP32-P4";; *) return 1;; esac; }

for sku in "${SKUS[@]}"; do
  id=$(sku_id "$sku") || { echo "unknown sku: $sku" >&2; exit 1; }
  echo "=== $id ($sku) $VERSION/$CHANNEL ==="
  if [ "$sku" = "t3" ]; then publish_t3 "$id"; continue; fi  # Linux SKU, different pipeline

  # Already published? Promote it — don't rebuild/re-sign over the live bytes.
  blob="keypad/${id}/${VERSION}.bin"
  if blob_exists "$blob" && [ -z "${FW_FORCE_REPUBLISH:-}" ]; then
    read -r sha size <<<"$(blob_digest "$blob")"
    python3 "$PUBLISH" --kind esp-fw --target "$id" --channel "$CHANNEL" \
      --version "$VERSION" --url "${BLOB_BASE}/${blob}" --sha256 "$sha" --size-bytes "$size" \
      --metadata-json "{\"chipFamily\":\"$(chip_family "$sku")\",\"flasher_url\":\"${BLOB_BASE}/keypad/${id}/flash/manifest.json\"}"
    echo "  promoted to ${CHANNEL} (existing artifact, $size bytes, sha256 ${sha:0:12}...) — no rebuild"
    continue
  fi
  [ -n "${FW_FORCE_REPUBLISH:-}" ] && blob_exists "$blob" && \
    echo "  WARNING: FW_FORCE_REPUBLISH — overwriting a published ${VERSION}; re-register every OTHER channel on this version or it will serve a stale sha256"

  bdir="$(build_dir "$sku").rel"   # release build dir (board.sh MMK_RELEASE=1)
  ./board.sh "$sku" build >/dev/null
  bin="${bdir}/mmkeypad_idf.bin"
  signed="$(mktemp -t "${id}-${VERSION}").bin"
  espsecure.py sign_data --version 2 --keyfile "$FW_SIGN_KEY" --output "$signed" "$bin" >/dev/null
  sha=$(shasum -a 256 "$signed" | awk '{print $1}')
  size=$(stat -f%z "$signed" 2>/dev/null || stat -c%s "$signed")
  az storage blob upload --account-name "$STORAGE_ACCOUNT" --account-key "$STORAGE_KEY" \
     --container-name "$CONTAINER" --name "$blob" --file "$signed" --overwrite -o none
  url="${BLOB_BASE}/${blob}"
  rm -f "$signed"

  # ESP32 firmware: plain signed .bin. Carry the chip family + (deterministic)
  # esp-web-tools flasher manifest url as metadata for the add-device wizard.
  python3 "$PUBLISH" --kind esp-fw --target "$id" --channel "$CHANNEL" \
    --version "$VERSION" --url "$url" --sha256 "$sha" --size-bytes "$size" \
    --metadata-json "{\"chipFamily\":\"$(chip_family "$sku")\",\"flasher_url\":\"${BLOB_BASE}/keypad/${id}/flash/manifest.json\"}"
  echo "  -> $url  ($size bytes, sha256 ${sha:0:12}...)"

  # ── In-browser flasher (esp-web-tools) ──────────────────────────────────────
  # A single MERGED first-flash image (bootloader + partitions + ota_data + app)
  # the add-device wizard flashes over Web Serial, plus a stable esp-web-tools
  # manifest. Blob layout: keypad/<id>/flash/{<version>-merged.bin, manifest.json}.
  # NOTE: this ships the current per-SKU firmware; once the open loader exists,
  # flash THAT here and let it OTA the full image .
  merged="$(mktemp -t "${id}-merged").bin"
  ( cd "$bdir" && esptool.py --chip "$(idf_chip "$sku")" merge_bin -o "$merged" "@flash_args" ) >/dev/null
  mname="keypad/${id}/flash/${VERSION}-merged.bin"
  az storage blob upload --account-name "$STORAGE_ACCOUNT" --account-key "$STORAGE_KEY" \
     --container-name "$CONTAINER" --name "$mname" --file "$merged" --overwrite -o none
  mf="$(mktemp).json"
  cat > "$mf" <<JSON
{ "name": "MMKeypad ${id}", "version": "${VERSION}", "new_install_prompt_erase": true,
  "builds": [ { "chipFamily": "$(chip_family "$sku")",
                "parts": [ { "path": "${VERSION}-merged.bin", "offset": 0 } ] } ] }
JSON
  az storage blob upload --account-name "$STORAGE_ACCOUNT" --account-key "$STORAGE_KEY" \
     --container-name "$CONTAINER" --name "keypad/${id}/flash/manifest.json" --file "$mf" \
     --content-type "application/json" --overwrite -o none
  rm -f "$merged" "$mf"
  echo "  -> flasher: ${BLOB_BASE}/keypad/${id}/flash/manifest.json"
done

echo "published to the OTA catalog at ${NUVOXEL_API_BASE:-https://nuvoxel.com}/api/v1/fw/publish — live immediately, no commit/deploy needed."
echo "flasher: ensure the '$CONTAINER' Blob container has CORS allowing GET from the web origin."
