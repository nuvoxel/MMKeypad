# Control4 glassedge factory firmware — analysis & restore reference

The authoritative, guaranteed-pristine Control4 factory image for the T3
(glassedge, RK3188). First non-unit reference we have — every other image on hand
is a per-unit dump of uncertain purity. Captured/analyzed 2026-07-25.

## What we have

- **Source (durable):** the Control4 factory image zip (`glassedge-full-image_3.2.1.589103-res.zip`)
  and a copy in `firmware-linux-t3/build/firmware/glassedge-3.2.1-589103-res/`
  (gitignored — Control4-proprietary). The zip holds `image_md5sums.txt` + a
  Rockchip **RKFW `update.img`** (1.05 GB).
- **Unpacked + verified partition images** in that same dir (each md5 matches the
  zip's own `image_md5sums.txt`):

  | image | size | md5 | nand LBA |
  |---|---|---|---|
  | `RK3188Loader_V2.15.bin` | 201038 | — | (loader) |
  | `misc.img` | 49152 | `756c8e2b3ef736c7957b3d537a12c93d` | 8192 (0x2000) |
  | `kernel.img` | 8822832 | `878097d5fb8b0174299f1094c2b4239e` | 16384 (0x4000) |
  | `boot.img` | 9977856 | `fb9b0cf1070de599374429e2e4bf5195` | 40960 (0xa000) |
  | `recovery.img` | 12222464 | `eb490ee11f71faaa29ba91286ee6f670` | 65536 (0x10000) |
  | `system.img` | 1073741824 | `53ff38d036092dd7257b804aba6f1d50` | 7618560 (0x744000) |

## Version — OLDER than our units

- C4 OS **3.2.1**, Android **4.4.2 `KOT49H`**, build **`589103-res`**, built
  **Tue Nov 17 2020**, `sys.rkadb.root=0` (clean, unrooted). Bootloader `V2.15`.
- The **units run `734549-res` / C4 4.0.0 (Mar 2025)** — newer. So:
  - Good enough to **restore a unit to a clean (older) factory** that boots.
  - **Cannot byte-diff** against the units' 734549 `/system` (different release).
    To verify "what's modified" on a unit, or keep it on 4.0.0, get the **734549**
    full-image and drop it in beside this one.

## It is a genuinely UNIVERSAL 7"/10" image (verified, not assumed)

The single kernel **detects the board at runtime** — it does not bake in one panel:

- Reads **four board-ID strap pins**: kernel strings
  `Reading board id0/id1/id2/id3 failed!` and `Board ID: %d (0b%d%d%d%d)`.
- Resolves the 4-bit ID to a model + revision: prints
  `Device type is: glassedge7 | glassedge7p | glassedge10 | glassedge10p` and
  `HW Rev is: %d`. The variant table in the kernel:
  `glassedge7 / 7.1 / 7.2 / 7p / 7p.1 / 7p.2 / 10 / 10.1 / 10.2 / 10p / 10p.2`.
- **Drivers self-configure from the detected type**: the GSL touch driver branches
  on it (`gsl: Device type is: glassedge10 … using new EST firmware`), and the LCD
  enable/backlight GPIO setup lives in the same board-detect routine.
- **Userland is model-aware too**: the Control4 launcher app carries configs for
  all four models (`glassedge7/10/7p/10p.conf` — present in Kitchen's backup),
  selected by the detected type.
- **Empirical corroboration on our own units**: both 10" units report
  `c4.device.type=glassedge10`, the 7" reports `glassedge7`, and each drives its
  correct native resolution — that property is downstream of exactly this routine.

**Audio + camera (built into the universal kernel, same 3.4.3/4.0.0):** Audio =
**ES8323 codec** (+ `rk610` companion) over `rk29_i2s`/ASoC, card 0 —
`audio_policy.conf` declares **built-in SPEAKER out + built-in MIC in** (so the
intercom/announce path has hardware). Camera = **NT99141 front sensor** via
`rk_cam_cif` + `rk29-ipp` + `soc_camera` (the kernel also carries ov2659/ov5640/ov5642
drivers, so it's multi-sensor-robust); `system/etc/permissions/rk-camera-front.xml`.
Both are **compiled into the kernel** (not `.ko` modules) — every glassedge kernel
gives audio+camera natively; our custom Linux just needs userspace (ALSA / V4L2). See
[CAMERA.md](CAMERA.md) for the working capture path.

**Caveat (honest):** the board-ID→device-type detection and the touch-firmware
selection are proven from kernel strings; the LCD enable GPIOs are in that same
path. The exact board-ID→LCD-**timing** mapping is compiled code, not strings, so
it's "proven mechanism + strong corroboration," not byte-traced. Every unit we own
boots its correct panel, so wrong-panel risk is low. The conclusive test is
empirical (flash → confirm 1280×800 on a 10") and fully reversible given backups.

## RKFW/RKAF layout (how it was unpacked)

`update.img` = RKFW wrapper (build 2020-11-17, chip RK3188) → embedded **RKAF** at
file offset **0x2fb20**. RKAF header: `num_parts` at `RKAF+0x88`, part entries at
`RKAF+0x8c`, each **0x70 bytes**: `name[32]`, `filename[60]`, then u32
`nand_size, pos, nand_addr, padded_size, size`. A part's data is at file offset
`RKAF_start + pos`. Parts: package-file, bootloader, parameter, misc, kernel, boot,
recovery, system, backup(RESERVED). `parameter` CMDLINE mtdparts is **identical to
our units** (same partition offsets) — images flash to the same LBAs.

No macOS `afptool`/`rkImageMaker` was needed; a manual RKFW/RKAF parse extracts it
(see this session's scratch script). `rkdeveloptool` only unpacks the bootloader.

## Restore recipe — "factory + root" on a unit (loader mode)

Flash the factory partitions as a **matched set** (kernel+system from the same
release ⇒ no kernel↔system skew — the failure that broke a standalone boot swap),
then re-root. With the unit in loader (`rkdeveloptool ld` shows `Loader`):

```
RK=.../rkdeveloptool/build/rkDevelopTool_Mac
D=.../build/firmware/glassedge-3.2.1-589103-res
$RK wl 8192    $D/misc.img
$RK wl 16384   $D/kernel.img
$RK wl 40960   $D/boot.img
$RK wl 65536   $D/recovery.img
$RK wl 7618560 $D/system.img          # 1 GB, slow over loader
# read-back verify boot/kernel, then:
$RK rd                                 # reset → boots clean factory 3.2.1
```

**Root** (same 4-line edit used on Kitchen; do it offline to a COPY of `system.img`
before flashing, via `debugfs`, preserving the selinux xattr):
`sys.rkadb.root 0→1`, `ro.adb.secure→0`, `persist.sys.usb.config→adb`,
`+ro.debuggable=1`. See [UNIT-INVENTORY.md](UNIT-INVENTORY.md) Kitchen section for
the exact `debugfs` procedure. Trade-off: this restores the unit to **3.2.1**
(downgrade from 4.0.0). Use a `734549` image instead to stay on 4.0.0.

## Getting ANY glassedge OS version from Control4's update server (the real key)

The Director does **not** cache the ~1 GB endpoint OS images — it pulls them from
Control4's update CDN via the Device Image Updater (`/mnt/internal/patchman/
C4_Device_Image_Updater_V2.sh`). The images are **publicly fetchable over HTTP**
(no auth) once you know the URL scheme (derived from the DIU script 2026-07-25):

```
http://update2.control4.com/<repo>/<OSVER>/<OSVER>.xml      # manifest (lists every file + md5 + size)
http://update2.control4.com/<repo>/<OSVER>/<fileName>       # each payload
```
- `<repo>`: **`experience`** for 4.x (4.0.0, 4.2.0), **`release`** for 3.2.1.
  (Others exist: release2, develop, beta…; DIU env→repo mapping in the script.)
- `<OSVER>`: `MAJOR.MINOR.PATCH.BUILD-res`, e.g. `4.0.0.734549-res`,
  `4.2.0.753182-res` (the Director's paired TS OS), `3.2.1.589103-res`.
- The manifest lists the T3 image as **`glassedge-ota_<OSVER>.zip`** (one universal
  zip for all four glassedge models — 10/10p/7/7p all point to the same file).
- The Mac can download these directly (verified). No Director round-trip needed
  once you have the URL.

### 4.0.0 image (matches our units) — HAVE IT, verified

`build/firmware/glassedge-4.0.0-734549-res/glassedge-ota_4.0.0.734549-res.zip`
(406 MB, md5 **`07a48e651545153eef471a2890d193d8`** = manifest checksum ✓).

**Different packaging from 3.2.1:** this is an **Android recovery OTA**, not an RKFW
`update.img`. Contents: `update-binary`+`updater-script` (Edify), raw **`boot.img`**
(md5 `3d1f9bd5…`, embedded kernel **`12b283f2` = the universal board-ID kernel the
units run**, has `Reading board id0`/`Device type is: glassedge10`), `recovery.img`,
and the full **`system/`** tree as files (pristine **unrooted** 4.0.0, `ro.c4.version
=4.0.0`, `734549-res`, `sys.rkadb.root=0`). `updater-script` gates on
`ro.product.device == "rk3188"` (panel-agnostic) and applies in misc-staged passes
(write recovery → reboot recovery → format+extract `/system` → patch boot).

**Two ways to put 4.0.0 on a unit:**
1. **Recovery sideload** (proper): boot the unit's recovery → `adb sideload
   glassedge-ota_4.0.0.734549-res.zip`. It's Control4-signed, so stock recovery
   verifies it. Multi-stage (misc flags) — the update-binary drives the reboots.
2. **Loader-flash reconstruct** (like we did 3.2.1): `wl` the raw `boot.img` +
   `recovery.img` directly; for `/system`, **build a 1 GB ext4 from the `system/`
   tree** (make_ext4fs + `file_contexts` for selinux labels) then `wl` it. More work
   than 3.2.1 (which shipped a ready `system.img`), but gives loader-flash control.

This is also the **pristine unrooted 4.0.0 `/system` reference** we lacked — the
`system/` tree (with per-file checksums in the OTA) is byte-diffable against the
units to prove factory-vs-modified.

## Full T3 (glassedge) firmware catalog + the T-series family

Authoritative source: **`sdlist.txt`** (`http://update.control4.com/patches/
CASSH_Manager/sdlist.txt`, preserved in `build/firmware/`) — one line per device:
`<device>,<minOS>,<maxOS>`. For glassedge: **min `2.8.2.515974-res`, max
`4.0.0.999999-res`** (so the T3 spans OS 2.8.2 → the last 4.0.0.x; nothing in 4.1/4.2).

**Every glassedge (T3) image that exists** (probed the CDN by real build numbers from
`sdlist.txt`; all under `update2.control4.com`; `glassedge-ota_<OSVER>.zip` unless noted):

| OSVER | repo | size | notes |
|---|---|---|---|
| `2.8.2.515974-res` | release | 355 MB | **HAVE** (`b9fe0450…`) — **earliest** (T3 launch OS; corrects a wrong "no 2.x" call) |
| `3.1.0.566775-res` | release | 428 MB | |
| `3.2.1.589103-res` | release | (full-image) | **HAVE** — RKFW full-image, flashed to eBay unit |
| `3.2.4.615802-res` | release | 302 MB | |
| `3.3.0.628678-res` | release | 363 MB | |
| `3.3.1.639488-res` | release | 410 MB | |
| `3.4.1.701303-res` | release | 425 MB | HAVE — intermediate 3.4.1 (NOT the last 3.x; my earlier error) |
| `3.4.1.705920-res` | release | — | (per DIU list) |
| `3.4.2.709259-res` | release | — | (per DIU list) |
| `3.4.3.727848-res` | release | 425 MB | **HAVE** (`dee9c7f9…`) — **last 3.x** and the official ceiling for the T3 (DIU max) |
| `4.0.0.734549-res` | experience | 406 MB | **HAVE** — newest build overall, but **experience-channel only; no supported tool installs it on a T3** |

Have on disk (`build/firmware/`): 2.8.2 (`b9fe0450…`), 3.2.1 (RKFW), 3.4.1
(`1863325e…`, intermediate), **3.4.3** (`dee9c7f9…`, last 3.x), 4.0.0 (`07a48e65…`),
T4 4.2.1, bcm7 (pre-T3). Others a `curl` away.

**Tooling reality (T3 is EOL, confirmed 2026-07-25):** device OS is version-matched to
the **Director**. The **DIU V2** (standalone) only lists up to **`3.4.3.727848`**
(Production/`release`). The newer **C4 ToolBox 4.2.0.3** explicitly lists **T3 Glassedge
as UNSUPPORTED** (with T4, Halo, Neeo, IO Extender, Home Controllers), and its "enhanced
connection" (JWT/REST, like the Director's `/api/v1/localjwt`) fails against a stock T3
(no endpoint, no cert patch). **`4.0.0.734549` IS installable via supported tooling —
but only from a Director on the 4.0.x train** (which pushes the matching device OS; this
is how the units originally got 4.0.0). Our Director is **4.2.1**, which dropped the T3,
so it can't; and the standalone DIU caps at 3.4.3. So for us the paths to 4.0.0 are: a
4.0.x Director (heavy), or our own **loader-flash reconstruct** (we have the image).
`3.4.3.727848` = easiest official latest. For NuVoxel the 3.4.3-vs-4.0.0 gap is
immaterial (we replace the OS anyway).

**Kernel/driver diff 3.4.3 vs 4.0.0 (measured):** **drivers are byte-identical** —
`mali.ko`/`rkwifi.oob.ko`/`rk29-ipp.ko` (and the whole module set) have the same md5
and vermagic in both. The **kernel was rebuilt** (4.0.0 = `12b283f2` `builder@linux-build-2`;
3.4.3 = `7664c227` `builder@linux-build-14`) but is the **same version** `3.0.36+`,
same size — a rebuild, not new hardware support, and the same modules load on both
(non-strict vermagic). ⇒ At the kernel/driver level 3.4.3 ≡ 4.0.0; the only real
differences are the Control4 userland apps. **No dev reason to chase 4.0.0.**

**Getting 4.0.0 — the DIU nuance (corrected 2026-07-25):** the shell
`C4_Device_Image_Updater_V2.sh` **runs ON the target device**, not remotely from the
Director — every step is device-side (`getprop`, `wget` of the payload, the flash tool,
`reboot`); `--ip` is effectively localhost (only ping+log), and `T3_FLAG` is set by the
on-device Android path `/mnt/secure/asec`. So you canNOT run it from the EA3 to push a
remote T3. The **remote** flasher is the **Windows GUI DIU** (it connects via Control4's
device-management channel — that's what read the eBay unit's info and offered Install),
but its version list is **Production/`release` only → caps at 3.4.3**. Real paths to
4.0.0: (a) set the Windows DIU's **environment to Early-Access/experience** if it exposes
that selector/config (then 4.0.0.734549 should list and the GUI flashes it properly); (b)
run the shell DIU **on the T3 itself** with `--env production --repo experience --version
4.0.0.734549-res --ip 127.0.0.1` — but that needs a shell on the T3 (root it first); or
(c) our **loader-flash reconstruct** (no DIU). `3.4.3` via the GUI is the easy path and is
kernel/driver-identical to 4.0.0 anyway.

**GUI DIU internals (inspected the Windows binary via Parallels `/Volumes/[C]…`):** repo
is **compiled into `C4_Device_Image_Updater_V2.exe`** (`.exe.config` is only the .NET
runtime decl). It bakes in only `update2.control4.com` (release) + `update2-beta.control4.com`
(beta) with an `AllowBeta` flag; **it never references the `experience` repo**, and the
beta host serves **no glassedge 4.0.0/3.4.3**. So the GUI reaches at most 3.4.3
(Production); 4.0.0 would require binary-patching the compiled URL/repo — not worth it.
Net: **DIU → 3.4.3 (GUI, one click) or loader-flash → 4.0.0 (ours) are the only real paths.**

**T4 hardware (what the `ts-t4` A/B payload targets):** NXP **i.MX8M Mini** (`imx8mm`),
**arm64/aarch64**, **Android** with A/B seamless `update_engine` + AVB verified boot
(payload partitions: `boot`, `system`, `vendor`, `vbmeta`, `dtbo`), **Linux kernel
6.1.55**, U-Boot, **Qt5** UI. A full generation beyond the T3 (RK3188 / Android 4.4.2
/ kernel 3.0.36).

### T-series device family (from `sdlist.txt` + the 4.2.1 manifest)

| device | SoC / arch | OS | update format | status |
|---|---|---|---|---|
| **T3** `glassedge7/7p/10/10p` | RK3188 ARMv7, Android 4.4.2 | 2.8.2 → 4.0.0 | recovery-OTA zip (`glassedge-ota`) **or** RKFW `update.img` (full-image); single-slot | **EOL** (dropped after 4.0.0; in `extinct.xml`) |
| **T4** `ts-t4-inwall10/8`, `ts-t4-tabletop10/8` | i.MX8MM arm64, Android | 3.2.4 → 6.0.0 (current) | **Android A/B `update_engine`** — `t4-ota-<ver>_payload.bin` (+`_payload_properties.txt`); dual-slot seamless | **current** |
| **T5** | — | — | — | **does not exist** in the catalog (no `ts-t5` in sdlist or the 4.2.1 manifest; touchscreens are only T4 + the EOL T3) |

**How they differ (the "how they work"):**
- **T3** is old single-slot Android: an update writes `boot` then formats and
  re-extracts `/system` (downtime, no rollback). Delivered either as a recovery OTA
  zip (update-binary/updater-script) or an RKFW `update.img` for loader flashing.
  The kernel self-selects the panel at runtime via 4 board-ID straps (universal).
- **T4** is modern seamless Android: `update_engine` applies a signed
  `payload.bin` (e.g. 4.2.1 payload = 583 MB; `payload_properties.txt` carries
  `FILE_HASH/FILE_SIZE/METADATA_HASH/METADATA_SIZE`, base64 SHA-256) to the
  **inactive A/B slot** in the background, then reboots into it — no downtime,
  automatic rollback on failure. Different SoC (i.MX8MM), arch (arm64), and a newer
  Android; the DIU detects it via `/system/u-boot-imx8mm.imx`.
- Same `update2.control4.com/<repo>/<OSVER>/` delivery for both; only the payload
  format differs. (The 4.2.1 catalog also lists a new `ubuntu` device — a
  Linux-based controller platform, separate from the touchscreens.)

## Where other Control4 device firmware lives (mapped 2026-07-25)

Two distinct channels — don't confuse them:

- **OS-image devices → the update CDN** (`update2.control4.com/<repo>/<OSVER>/`,
  per-version XML manifest). These are the full-OS endpoints: controllers
  (ea/core/ca…), **T3 glassedge**, **T4 `ts-t4`** (A/B `update_engine` payload).
  Enumerated by `sdlist.txt`.
- **MCU / peripheral firmware → on the Director** at `/control4/firmware/` (+ `pro/`,
  `wired/`, `io/`, `zwave/`, `ota/`). Small `.bin`/`.ebl` blobs for microcontrollers
  (AT128, EM357 Ember zigbee, TM4C, S25FL serial-flash). Confirmed present:
  - **SR250/SR260 remotes**: `700-00086_AT128_SR250RemoteControl…fw.bin`,
    `700-00119_AT128_SR250BRemoteControl…`, `720-xxxxx_S25FL164_SR260Remote_
    SerialFlash_OTA_Release_2.2.50…bin` (+ older SR150/SR150B).
  - **Keypads**: `700-00187_EM357_Keypad_Router…ebl` (zigbee),
    `700-00189_EM357_WiredKeypad_Release_5.0.82…ebl` (wired). (No separate "LCD
    keypad" OS firmware — the LCD keypads are these EM357-class MCUs.)
- **Neeo / Halo / Halo Touch remotes → neither channel.** Not in `sdlist.txt`, not in
  the 4.2.1 device manifest, not in `/control4/firmware/`. They self-update from
  Control4's cloud via their own driver/mechanism (separate path, not mapped here).

### Touchscreen driver model (relevant to the keypad project)

The Director's driver catalog has a device-specific `uidevice_glassedge.c4l` **only for
the T3**; **T4 and T5 use the generic `uidevice.c4l`**. So Control4 special-cased the
T3 but standardized every touchscreen after it onto the generic uidevice interface —
the same `uidevice` model the mmkeypad project studied (see the keypad-as-uidevice
finding: Director instantiates a uidevice + renders its settings UI but dispatches no
commands to it). A NuVoxel keypad presenting as a generic `uidevice` would look, to
the Director, like a T4/T5 — worth revisiting for the keypad's Director integration.

### Pre-T3 touchscreen (bcm7 / "In-Wall Touch Screen") — HAVE

`build/firmware/bcm7-pre-t3-2.9.1-532460-res/` — the T3's predecessor: **`bcm7`/`bcm7p`**
7"/10" In-Wall Touch Screen. **Kernel 2.6.32.9, ARMv6, 800×480, Flash-based navigator**
("flash-navigator" packages). Last version **2.9.1.532460-res** (its maxOS). Delivered
as debs + `rootfs-image-bcm` (75 MB, `79924978…` ✓), not a single OTA zip.
("Infinity Edge" is the T3's own design name, not this predecessor.)

## Stock runtime reference (from the eBay unit booted on 3.2.1, 2026-07-25)

Confirmed healthy stock via the on-device **`sysman`** console (Control4's
management/provisioning interface — reachable when the unit is up on the network):
`devicetype=glassedge10`, `system-version=3.2.1.589103-res`,
`hostname=glassedge10-000fffxxxxxx`, eth0 DHCP `<panel-ip>`, mac `…81:70:07`.

- **`sysman` command set** (the provisioning surface): `configure set|get|clear uri
  ['json']`, `net`, `ntp` (points the device at the master controller), `feature`
  enable/disable, `c4global`, `brightness`/`adaptivebrightness`, `camera`,
  `screensaver`, `snapshot` (logs+data, optional copy-to-master), `sysinfo`/`version`
  (XML), `procpoll`, `reboot`/`shutdown`, `enable`/`disable`/`status` (daemons),
  `uuid`, `whoami`. This is how a stock unit is configured without a UI.
- **Key stock C4 app versions (3.2.1):** Control4 (phoenix) `320.45.1.8`, Launcher
  `320.45.0.37`, Device Administrator `320.45.0.38`, **Update Manager (upmand)
  `320.44.0.9`**, SDDP `320.43.0.21`, Component Navigator `320.43.0.37`,
  C4Settings Provider `320.43.0.38`. Android base `4.4.2-589103-res`.
- **Path to 4.0.0:** the OS update is driven by **`upmand`** (Update Manager). A
  standalone/unpaired unit does not self-update — it gets the OS either (a) by being
  added to a Control4 project so a 4.0.0-era **Director pushes** the matching OS
  (SDDP discovery + Composer Pro), or (b) by us **flashing the `734549` full-image
  directly** the same way we flashed 3.2.1 (no Director needed). If the 734549 image
  is obtainable, direct-flash is the simplest route to 4.0.0 and also gives us the
  pristine-4.0.0 reference we still lack.

> Bootloader note: this set includes `RK3188Loader_V2.15.bin`. Flashing loader is
> only needed for a true full-factory reset; the boot/kernel/system set above is
> enough to return a unit to a clean, bootable factory OS. Don't flash the loader
> unless you intend a complete reset (and know the unit isn't newer-loader-locked).
