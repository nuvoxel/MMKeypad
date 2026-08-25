# MMKeypad firmware updates

The open firmware talks to no online service, so there is no cloud check-in,
manifest, or auto-update policy. Firmware reaches a device three ways:

1. **On-screen, from GitHub Releases** — the primary path, on every board
   including the T3.
2. **USB flashing** — the fallback, and how you flash the very first image.
3. **From source** — always available, and still how a T3 gets its first image.

## On-screen updater

On the panel: **Settings → Check for update**. The keypad queries this project's
GitHub Releases, lists the assets published for **the image it runs**
(`device_fw_image_id()` → `mmk-s3` / `mmk-poe` / `mmk-nano` / `mmk-ws43` /
`mmk-t3`), and installs the one you pick — it downloads the asset over HTTPS, writes it to the inactive
OTA partition (`esp_https_ota`), and restarts into it. The running version is
marked *installed* in the list.

Implementation: [`firmware-idf/main/fwupdate.c`](firmware-idf/main/fwupdate.c)
(GitHub Releases client + apply) and the picker overlay in
[`firmware-idf/main/ui.c`](firmware-idf/main/ui.c). The releases endpoint and the
asset-name convention are the only contract:

```
GET https://api.github.com/repos/nuvoxel/MMKeypad/releases
asset name:  <sku>-<version>.bin      e.g.  mmk-s3-2026.08.24.001.bin
```

## Publishing a release

Build the image for each SKU and attach them to a GitHub Release named for the
version. The asset name is load-bearing (the on-screen picker filters by
`<sku>-` prefix and detects the installed version from the version substring).

```sh
cd firmware-idf
VER=2026.08.24.001
for b in s3 poe nano ws43; do ./board.sh $b build; done
# rename each build/*/mmkeypad_idf.bin to mmk-<sku>-$VER.bin, then:
gh release create v$VER --repo nuvoxel/MMKeypad --target main \
  mmk-s3-$VER.bin mmk-poe-$VER.bin mmk-nano-$VER.bin mmk-ws43-$VER.bin
```

Version scheme is the shared date-based `YYYY.MM.DD.NNN` + `FW`
(`tools/nvversion.sh`); `firmware-idf/version.txt` is what the build stamps as
`fw_version()`, so the device reports exactly what the release tag says.

Integrity/authenticity: the download is TLS-authenticated to `github.com` /
`objects.githubusercontent.com` against the bundled CA roots, and `esp_https_ota`
validates the image header and app descriptor before it boots. (ESP Secure Boot
is not enabled in the open build.)

## USB flashing

The fallback, and how a fresh board gets its first image:

```sh
cd firmware-idf
./board.sh ws43 -p /dev/cu.usbmodemXXXX flash monitor   # or s3 | poe | nano | matrix
```

## T3 (RK3188 Linux) updates

The T3 updates from Releases like the ESP boards, but its artifact is different:
a **`t3-bundle` tar** (`mmk-t3-<version>.tar`) holding `meta.json`, the app, and
the init overlay — not a flashable app image. The updater accepts `.tar` for this
build and `.bin` for the ESP boards.

Applying it is the persistent app/init **overlay swap** in
[`firmware-linux-t3/platform/nv_ota_t3.c`](firmware-linux-t3/platform/nv_ota_t3.c):
download → sha256 → swap `/data/mmkeypad` + `/data/init.overlay` → reboot, with
crash-loop rollback to the previous binary.

Two things this does **not** cover. The bundle carries the app and init only, so
a change to `boot.img` — kernel, mounts, vendor modules — still needs a full
reflash from source. And a panel has to already be running a build that
understands `.tar` assets before it can see a T3 release at all; earlier builds
filtered for `.bin` and matched on the specific SKU (`mmk-t3-10`) rather than the
shared `mmk-t3` image, so they list nothing. Get to a current build once from
source — see [`firmware-linux-t3/README.md`](firmware-linux-t3/README.md) — and
it is self-updating from then on.
