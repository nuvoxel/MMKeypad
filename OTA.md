# MMKeypad OTA — publishing & auto-update

How firmware reaches devices in the field, for every keypad SKU. One control
plane (check-in → manifest → policy → download → verify → apply → rollback); two
install mechanisms (ESP native A/B, T3 split-init overlays).

## The shared control plane

Every device, on a periodic **check-in** to `DEVICE_CLOUD_URL` (`https://nuvoxel.com`),
sends its **identity + SKU + current `fw_version`**. The platform's `/api/v1/fw/check`
resolves the SKU → firmware image id (`SkuDef.fw`) → the newest entry for the
device's channel in the **firmware manifest**, and returns
`{update_available, version, url, sha256, policy}`.

- **`policy`** (platform/station setting) gates auto-apply: `auto` = download +
  apply now; `notify` = surface it but wait; `off` = ignore. The device only
  self-updates on `auto` (or an explicit "check now").
- Update happens on the **next check-in after** a newer version is published +
  the platform is deployed — within the check-in interval, not instantly.
- Integrity is **sha256** over HTTPS; authenticity is the committed manifest
  (+ ESP Secure Boot v2 signature on ESP images).

## Publishing (all SKUs): `firmware-idf/tools/publish-fw.sh`

```
publish-fw.sh <version|auto> <channel> [sku ...]
  publish-fw.sh auto beta t3          # T3 only, today's next seq
  publish-fw.sh auto stable           # every SKU (s3 poe nano ws43 matrix t3)
  publish-fw.sh 2026.07.18.001FW beta t3   # explicit version
```

Per SKU it builds a **release** image, hashes it, uploads to the public Blob
container (`keypad/<fw-id>/…`), and records `{version, url, sha256, size,
format, formatVersion}` in the manifest (`../nuvoxel/apps/web/public/fw/
manifest.json`, top-level `schemaVersion`). **Then commit + deploy `nuvoxel`** to
serve it (the manifest is a static asset the check handler reads).

Prereqs: `FW_STORAGE_KEY` or `az login` (Blob upload); `FW_SIGN_KEY` **only** when
an ESP SKU is in the set (the T3 is sha256-only, no Secure Boot). Version scheme
is the shared date-based `YYYY.MM.DD.NNN` + `FW` (`tools/nvversion.sh`); `auto`
advances `version.txt`, which the build stamps as `fw_version()` so the device
reports EXACTLY what the manifest records (the OTA "is it newer?" compare).

## ESP SKUs (`mmk-s3`, `mmk-poe`, `mmk-nano`, `mmk-ws43`, `mmk-matrix` — art-only, no touch/audio)

- **Mechanism:** native ESP-IDF OTA. The app is a single monolithic image flashed
  to an inactive **A/B partition** (`ota_0`/`ota_1` + `ota_data`); on the next
  boot it runs, and a healthy run marks it valid — a boot failure **rolls back**
  to the previous partition (`esp_ota` rollback). Full-image update, no separate
  kernel/init to worry about.
- **Artifact:** a Secure-Boot-v2-**signed** `.bin` (`format: esp-bin`). Publish
  also emits a merged first-flash image + an esp-web-tools manifest for the
  in-browser add-device flasher.
- **What updates:** everything (the whole firmware). Bootloader stays.

## T3 SKU (`mmk-t3`, incl. size variants `mmk-t3-7`/`-10` → same `fw: mmk-t3`)

The T3 is repurposed Control4 glass running our **Linux** userspace, so there's
no monolithic app to A/B-swap — there's a kernel, an init, and an LVGL app. We
make the two things that ever change **OTA-updatable via `/data` overlays**, and
leave the (stock, never-changing) kernel fixed:

- **Split init (`init.c`, one binary, two roles):** a **bootstrap** baked into
  `boot.img` (PID 1, never updated) does the can't-fail bring-up — mount
  `/dev`/`/proc`/`/sys`, NAND, `/data`, `/system`, network + dropbear + USB serial
  so the unit is **always reachable** — then execs `/data/init.overlay` if
  installed + healthy, else runs a built-in worker. The overlay (same binary,
  `"overlay"` arg) runs the worker (wifi detect+load, app launch/respawn/OTA) and
  commits itself after `INIT_CONFIRM_SEC`. A crashing overlay (PID1 panic →
  auto-reboot; we set `kernel.panic`) climbs a **trial counter** until the
  bootstrap quarantines it (`.bad`) and falls back to built-in — no brick, no
  manual power-cycle. The boot partition is **never rewritten at runtime**.
- **App overlay (`/data/mmkeypad`):** init prefers it over the factory
  `/usr/bin/mmkeypad`; crash-loops roll back to `.prev`/factory.
- **Artifact:** a **versioned tar bundle** (`format: t3-bundle`, `formatVersion`)
  carrying `meta.json` + `mmkeypad` (app) + `init.overlay` (init). `nv_ota_t3.c`:
  download → sha256 → **gate on `formatVersion`** (refuse a format newer than
  `T3_BUNDLE_MAX`) → extract → install both overlays (each keeping `.prev`) → if
  the init overlay changed, reset its trials + **reboot** so the bootstrap runs
  the new init under rollback; else exit for PID 1 to respawn the app.
- **What updates:** app + init (every real release). **Fixed:** the stock RK3188
  kernel — byte-identical across units, panel/peripherals auto-detected at
  runtime, so it never needs to change. (If it ever did: a `flash.sh --net`
  reflash, not OTA — see `firmware-linux-t3/README.md`.)

## Versioning (so schema/format can evolve safely)

- **`schemaVersion`** (manifest, top-level) — the manifest structure. Bump on a
  structural change; tooling/platform gate on it.
- **`formatVersion`** (per entry + inside the T3 bundle's `meta.json`) — the
  artifact layout. The device **refuses** a bundle whose `formatVersion` exceeds
  what it understands (`T3_BUNDLE_MAX`) rather than mis-installing — so an old
  device is safe against a newer format.
- Bump-gated in the publisher via `MANIFEST_SCHEMA` / `T3_BUNDLE_VERSION`.

## Releasing, end to end

1. `publish-fw.sh auto <channel> [skus]` — builds, uploads, updates the manifest,
   bumps `version.txt`.
2. Commit `version.txt` (+ `publish-fw.sh` if changed) in this repo; commit the
   manifest in `nuvoxel` and **push** — CI (`.github/workflows/deploy.yml`)
   deploys and the new manifest is served.
3. On their next check-in, `auto`-policy devices pull + apply + (self-)rollback on
   failure. ESP via A/B; T3 via app+init overlays.
