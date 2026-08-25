# T3 dev-unit inventory — conclusive per-unit differences

Goal: nail down exactly how our dev units differ, in **stock firmware**, so the
web flasher / firmware can be made hardware-rev aware. Motivated by tonight's
finding that two "identical `glassedge10.2.0`" T3-10s had **different NAND**
(MICRON vs HYNIX) and rejected/accepted the vendor `3.0.8` modules differently.

## The dev units

- **3× T3-10** (one possibly fried by an earlier incident).
- **1× T3-7**.

## Data points to capture per unit (the fingerprint)

Hardware (invariant, the ground truth):
- **NAND**: manufacturer + page/block size (loader `rfi`, cross-check `/proc/mtd`)
- **WiFi/BT chip** (`/sys/class/rkwifi/chip`, dmesg)
- **Touch controller** (`/proc/bus/input/devices`)
- **Camera sensor** (dmesg `sensor_probe`, `/dev/video*`)
- **Panel** (fb geometry, dclk/fps, native orientation)
- **RAM** (`MemTotal`)
- **efuse chip UID** (`/sys/devices/system/cpu/efuse_val`)
- **MAC / serial** (Control4 OUI `00:0f:ff:<serial>`)

Firmware (what Control4 shipped — messier, several units already modified):
- **Kernel**: md5 of the kernel region (unpack boot.img), `uname -v` string,
  **CONFIG_MODVERSIONS?** (decides whether the `3.0.8` vendor blobs load)
- **/system build date/version** (`ro.build.date`, `.incremental`)
- **Vendor modules present + their vermagic** (`rk29-ipp`, `mali`, `ump`,
  `vpu_service`, `rkwifi.oob`)

## How to probe

1. **Loader mode** (`rkdeveloptool ld`): `rfi` → definitive NAND chip; `rl 40960
   24576 boot.img` → kernel region for md5 + vermagic + MODVERSIONS check.
2. **Live boot** (rooted stock via `adb`, or our Linux via ssh/serial): run
   [`tools/probe-unit.sh`](tools/probe-unit.sh) — it dumps the whole fingerprint
   in one labeled block. Driver-level fields (WiFi/touch/camera chip, loaded
   modules) need a live boot; hardware is the same under stock or our Linux.

> **Caveat: several units are no longer in stock firmware.** We've flashed our
> custom Linux and/or swapped boot/system on some. Hardware fields are still
> conclusive regardless; for the *stock kernel/module* fields, prefer units we
> haven't touched (the 7" and the un-modified 10"s), or read the stock kernel
> from a preserved boot backup.

## Comparison table (fill as probed — CONFIRMED vs TBD)

| field | Unit A "Kitchen" 10" | Unit B "eBay" 10" (HYNIX) | Unit C "dead" 10" | Unit D "Playroom" 7" |
|---|---|---|---|---|
| MAC (`00:0f:ff:`) | `…82:a3:a6` ✅ | `…81:70:07` ✅ | `…82:6e:f8` ✅ | `…81:ca:08` |
| NAND | **MICRON** ✅ (flash ID `2c 64 44 4b a9`) 8KB/2048KB | **HYNIX** ✅ (loader value=02; live flash ID `ad de 94 eb 74`) 16KB/4096KB, 8528 MB | TBD | TBD |
| kernel md5 (region) | `12b283f2…` *(our repack — stock TBD)*; live mtd1(kernel)=`8fdd05bf…` mtd2(boot)=`200ba305…` | **running = cross-flashed** `linux-build-2` kernel (`12b283f2`, in boot `e1663ddc` = Kitchen's build). Its **own factory** boot was `3f68fac8` (`linux-build-14`, kernel `4bb2c593`). mtd1 kernel partition = `74c334a7` | TBD | TBD |
| `uname -v` | `#1-glassedge10.2.0` (Mar 13 2025) ✅ | `#1-glassedge10.2.0` (Mar 13 2025, `linux-build-2`) ✅ | TBD | `#1-glassedge7.2.0` (Mar 13 2025) |
| vendor-blob load (3.0.8+ on 3.0.36+ kernel) | **STOCK: loads** ✅ (mali/ump/rk29_ipp Live; kernel accepts mismatched vermagic). Our *repacked/strict* path is what rejected them — not the stock kernel. | **loads** ✅ (mali/ump/rk29_ipp/vpu Live at vermagic 3.0.8+ on the linux-build-2 kernel) — confirms it's a *kernel-build* trait, not NAND | TBD | TBD |
| WiFi chip | sysfs `OOB_RK903` / dmesg `AP6330(OOB)` ✅ (same BCM4330 core) | sysfs `OOB_RK903` ✅ | TBD | AP6330 *(confirm)* |
| touch | **Goodix** ✅ (`goodix-ts`) | **Goodix** ✅ (`goodix-ts`) | TBD | GSL1680 *(confirm)* |
| camera | **NT99141** ✅ (`nt99141_front_3`, `/dev/video0`) | **NT99141** ✅ (`/dev/video0`) | TBD | TBD (7" may differ) |
| panel fb | 1280×800 @16bpp landscape ✅ | 1280×800 @55 Hz (virtual 1280×2400) ✅ | TBD | 800×1280 portrait |
| RAM | 1 GB ✅ | TBD (probe) | TBD | TBD |
| efuse UID | `524b1388 51fae207 061c040c 030d0144 fa241700 00c13000…` | `524b1388 51fae007 08060613 240d0117 fa241700 00b63002…` ✅ | TBD | TBD |
| bootloader | `bootver=2013-12-27#2.10`, `firmware_ver=4.4.2` (cmdline) | TBD | TBD | TBD |
| current state | **STOCK Control4 4.0.0, ROOTED** (reflashed from own backup 2026-07-25); root adb up as `000fffyyyyyy` | **CLEAN FACTORY STOCK 3.2.1, unrooted** (flashed from official image 2026-07-25, booted OK); can go to 3.4.3 via DIU (latest supported) or 4.0.0 via DIU-CLI `--repo experience`. Prior rooted-C4 state saved at `backup-hynix-ebay-20260725/` | **DEAD — hardware fault** (2026-07-25): powers (camera LED) but never enters USB loader/maskrom/serial in ANY mode, same cable/process as the working units + cold-reset+button. Maskrom is in SoC boot ROM & should enumerate even with dead NAND ⇒ fault is recovery-button strap or USB-OTG PHY. USB-recovery exhausted; **set aside**. **MAC `826ef8`** — this is the unit with the MOST complete backup: `backup-000fffxxxxxx/` has every partition (incl `system.bin` + rooted `system_mod.bin`) + jailbreak artifacts (`build.prop.mod`, `ctx.bin`), so if it's ever revived via a test-point maskrom it's fully restorable. (It was the first unit jailbroken, ~Jul 13–14; died sometime after.) | our Linux (on WiFi, `<panel-ip>` as of 2026-07-25) |

**Confirmed tonight:** the MICRON vs HYNIX NAND split on the two 10"s, and that
the `12b283f2` kernel is strict-vermagic while the `61cd5393` kernel has
MODVERSIONS. Everything marked *(confirm)* is from earlier-session probing and
should be re-verified per-unit; **TBD** needs a fresh probe.

### Unit A "Kitchen" — probe 2026-07-25 (via USB serial console, our Linux)

Full capture highlights (channel: `/dev/cu.usbmodem201303`, root shell on
`ttyGS0`; unit was off-network — no `wlan0`, only `rk30xxnand_ko` loaded):

- Kernel: `Linux 3.0.36+ #1-glassedge10.2.0 SMP PREEMPT Thu Mar 13 02:06:56 MDT 2025`
- NAND flash ID `2c 64 44 4b a9` → Micron (0x2c). mtd map: misc/kernel/boot/
  recovery/backup/cache/userdata/metadata/kpanic/system/user (erasesize 0x4000
  via FTL).
- Live-read md5s (through `/dev/mtd*` FTL — **not directly comparable to loader
  `rl` reads**; loader-mode ground truth still TBD): mtd1 kernel
  `8fdd05bf5f8fd89ba40206d7f204a8c7`, mtd2 boot `200ba305cf6849977bb42ef7e53d1ef1`.
- Module inventory: `/system/lib/modules` still carries the stock vendor set —
  mali/ump/rk29-ipp/vpu_service are vermagic **3.0.8+** (won't load on this
  kernel); everything else 3.0.36+. `/lib/modules` has our rebuilt
  mali/ump/rk29-ipp/rk30_mirroring/vpu_service at vermagic **3.0.36+**.
- WiFi: `/sys/class/rkwifi/chip` reads `OOB_RK903` while the driver logs
  `Current WiFi chip is AP6330(OOB)` (a poller reads the node every ~35 s).
  Both names are the same Broadcom BCM4330-core module; treat as one part.
- Camera present and probing: `Probe nt99141_front_3 success`, `/dev/video0`.

### Unit A "Kitchen" — REFLASHED to stock Control4 (rooted) 2026-07-25

Reverted from our Linux back to **pristine stock Control4, then rooted**, entirely
from *this unit's own* verified backup (`~/git/MMKeypad/firmware-linux-t3/build/backup-10-000fffyyyyyy/`).

**What was flashed** (rkdeveloptool `wl`, loader mode; all read-back verified):

| partition | LBA | source | md5 |
|---|---|---|---|
| misc | 8192 | own `misc.img` | `bcb87d36…` |
| kernel | 16384 | own `kernel.img` | `a41831866c…` (read-back ✅) |
| boot | 40960 | own `boot.orig` | `e1663ddc…` (read-back ✅, = documented true-stock 10" boot) |
| recovery | 65536 | own `recovery.img` | `500586ae…` |
| backup | 131072 | own `backup.img` | `8317cb79…` |
| system | 7618560 | own `system.img` **+ root edit** | `9319db10…` (rooted) |

**Root** = a 4-line edit to `/system/build.prop`, applied offline to *Kitchen's own*
`system.img` (not the sibling's `system_mod.bin`, whose /system differs — Kitchen
stock system `71aa8ca0…` ≠ sibling stock `9de504b8…`). Edit via `debugfs` on a
scratch copy (backup left pristine): `sys.rkadb.root 0→1`, `ro.adb.secure 0`,
`persist.sys.usb.config mass_storage→adb`, `+ro.debuggable=1`; preserved the
`security.selinux = u:object_r:system_file:s0\0` xattr (= `ctx.bin`) and 0644 root:root;
`e2fsck` clean. Identical change-set to the sibling's known-good `system_mod`.

**Stock config as inventoried live (rooted adb `uid=0`):**
- **Control4 OS `ro.c4.version = 4.0.0`**; `c4settings` pkg `324.23.0.7`; device
  `glassedge10`, **hwrev 2**; hostname `glassedge10-000fffyyyyyy`; intercom client
  feature **enabled**; director **unknown** (unpaired bench unit).
- **Android** 4.4.2 `KOT49H` / `734549-res` / `rockchip/rk3188/rk3188…release-keys`.
- **Running kernel** = the **Mar 13 2025 `#1-glassedge10.2.0`** build carried in the
  *boot* partition. ⚠️ The standalone `kernel` mtd (mtd1, md5 `a41831866c…`) holds a
  **different, older Sep 1 2017 `#1-glassedge.0`** kernel (`builder@f5c6daa08ef3`)
  that the normal boot flow does **not** use — a fossil from before an OTA rewrote
  `boot`. This is the source of the earlier "region A = 2017 vs region B = 2025"
  discrepancy. *Original kernel Control4 actually runs on glassedge10 = the Mar 2025
  one.*
- **Modules that stock loads** (`/proc/modules`): `rk29_ipp`, `mali`, `ump`,
  `rk30xxnand_ko` — all **Live**, despite `mali/ump/rk29_ipp` being **vermagic
  `3.0.8+`** on a **`3.0.36+`** kernel. ⇒ **the stock kernel does not enforce
  vermagic** and loads the vendor graphics/media blobs fine. (`no /proc/config.gz`,
  so CONFIG_MODVERSIONS isn't directly readable, but the load itself is the proof.)
  Corrects the earlier "Kitchen rejects 3.0.8 blobs" note — that was our repacked
  strict-vermagic path, **not** stock.
- WiFi `AP6330(OOB)` / sysfs `OOB_RK903`; camera `nt99141` (`/dev/video0`); touch
  `goodix-ts`; panel native **1280×800 @55 Hz** (fb virtual 1280×2400 = triple-buffer).
- efuse UID `524b138851fae207061c040c030d0144fa24170000c13000…` and MAC
  `00:0f:ff:xx:xx:xx` — unchanged, reconfirmed under stock.
- init NTP is pinned to `0.control4.pool.ntp.org`; stock services (`c4core`/`c4init`)
  present but **stopped** (not paired to a director).

Full raw capture: scratchpad `kitchen-stock-inventory.txt` (this session).

### Unit B "eBay" HYNIX 10" (000fffxxxxxx) — first full backup + inventory 2026-07-25

The eBay-bought 10" (HYNIX NAND). Until today we had **only a stale `boot.orig`** for
it; its original `/system` was never captured (the "eBay-10 failure": its own
`/system` version-skewed and killed wlan0/rndis/adb, worked around with
`flash.sh --write-system`). Connected in loader today → **took the full backup we
were missing**, then booted it to fingerprint.

**Full loader backup** → `firmware-linux-t3/build/backup-hynix-ebay-20260725/`
(misc/kernel/boot/recovery/backup/metadata/kpanic/system + loader_region +
parameter.txt). `system.img` `e2fsck`-clean. Partition hashes:

| part | md5 | note |
|---|---|---|
| misc | `71048c16…` | own |
| kernel (mtd1) | `74c334a7…` | own; **differs** from Kitchen's `a41831866c` |
| boot | `e1663ddc…` | = Kitchen/826 stock boot → **cross-flashed**, not its own `3f68fac8` |
| recovery | `500586ae…` | = Kitchen's |
| backup | `8317cb79…` | = Kitchen's |
| system | `6916a91f…` | **unique** (not a Kitchen/826 donor copy); **ROOTED** |

**Current state = rooted C4 4.0.0, NOT factory:**
- `build.prop` carries the same 4 root edits we use (`sys.rkadb.root=1`,
  `ro.adb.secure=0`, `persist.sys.usb.config=adb`, `ro.debuggable=1`) + selinux
  xattr. So the captured `system.img` is a **rooted** system — we still do **not**
  hold this unit's original *unrooted* `/system`.
- Running kernel is the **linux-build-2** `#1-glassedge10.2.0` (from the
  cross-flashed `e1663ddc` boot), **not** its factory `linux-build-14` kernel
  (its own `3f68fac8` boot, preserved in `backup-000fffxxxxxx/boot.orig`). Two
  deviations from factory: rooted system **and** swapped kernel/boot.
- Loads the 3.0.8+ mali/ump/rk29_ipp/vpu blobs Live (like Kitchen) — because it's
  running the same non-strict linux-build-2 kernel; confirms the vermagic behaviour
  tracks the **kernel build**, not the HYNIX vs MICRON NAND.
- HW profile identical to Kitchen: RK903/AP6330 WiFi, NT99141 camera, Goodix touch,
  1280×800@55 Hz. efuse UID `524b138851fae00708060613240d0117fa24170000b63002…`,
  MAC `00:0f:ff:xx:xx:xx`. Live NAND flash ID `ad de 94 …` (0xad = Hynix).

Raw capture: scratchpad `ebay-817007-inventory.txt`.

**Factory-boot restore attempt 2026-07-25 — FAILED, reverted.** Flashed its
archived own factory boot `3f68fac8` (`linux-build-14`) over loader (read-back
verified) to undo the cross-flash while keeping the rooted system. On reset it
**boots to recovery, not normal Android** (`adb …=recovery`, no shell). `misc`
BCB was clean (all-zero command, not forcing recovery), so the `3f68fac8` image
itself does not yield a working normal boot with the current `/system` — either
the kernel↔system pairing skew (the original eBay failure) or that undocumented
`boot.orig` isn't a clean normal-boot image. Reverted to the working `e1663ddc`
boot (read-back verified) → back to rooted C4, adb device-mode, WiFi up. **Takeaway:
don't restore `3f68fac8` standalone; the correct factory boot must come paired with
a matching factory `/system` — i.e. from the official Control4 `.bin`.**

**Flashed to FULL FACTORY STOCK 3.2.1 (unrooted) 2026-07-25** — matched set from the
official image ([FACTORY-IMAGE.md](FACTORY-IMAGE.md)): `misc`+`kernel`+`boot`+
`recovery`+`system` via loader (NOT loader-bin, NOT rooted). All read-backs matched
factory hashes exactly (boot `fb9b0cf1`, kernel `878097d5`, system `53ff38d0`). On
reset it went to **recovery first** (expected: factory `/system` over the old 4.0.0
`/data`+`/cache` → recovery formats on first boot), then recovery adb dropped → no
adb/no loader (factory stock has no adb by design). Goal: user OTA-updates it to
4.0.0 from the clean factory. **Result: booted clean into factory OS 3.2.1** (the
brief recovery pass formatted the stale data as expected). Unit is now pristine
unrooted factory stock, ready for the Control4 OTA to 4.0.0. Full prior-state revert
(rooted-C4) available at `backup-hynix-ebay-20260725/`.

> **The authoritative fix for "what did Control4 ship" is the official Control4
> factory image `.bin`** — worth obtaining. None of our unit dumps are guaranteed
> pristine (this one is rooted + cross-flashed; Kitchen's is the cleanest but still
> a per-unit dump). Diffing `system.img` against the official image will tell us
> exactly what's factory vs modified on any unit, resolve the linux-build-2 vs
> linux-build-14 kernel question, and give a true-factory restore path.

## Official Control4 factory image (authoritative reference)

the Control4 factory image zip (`glassedge-full-image_3.2.1.589103-res.zip`) — a genuine Control4
factory OTA (RKFW `update.img`, 1.05 GB). Unpacked + **every image verified against
its own `image_md5sums.txt`** (2026-07-25). This is the first non-unit, guaranteed-
pristine reference we have.

- **Version:** C4 OS **3.2.1**, Android 4.4.2 `KOT49H`, build **`589103-res`**,
  built **Nov 17 2020**, `sys.rkadb.root=0` (clean stock). Bootloader
  `RK3188Loader(L)_V2.15.bin`.
- **⚠️ Older than our units.** Units run **`734549-res` / C4 4.0.0** (Mar 2025).
  So this image's `system` will **not** byte-match the units — good enough to *restore*
  a unit to a clean (older) factory, but **cannot byte-diff** against the 734549 units.
  To verify "what's modified" or match our units, get the **734549** full-image.
- **Universal 7"/10" (verified mechanism, not just string match).** The kernel
  (`builder@linux-build-1`) reads **four board-ID strap pins** at boot
  (`Reading board id0..3`, `Board ID: %d (0b%d%d%d%d)`) → resolves to
  `Device type is: glassedge7|7p|10|10p` + `HW Rev`; drivers self-configure (GSL
  touch branches on it; LCD en/backlight GPIOs in the same routine). Our units'
  `c4.device.type` (glassedge10 on the 10"s, glassedge7 on the 7") is downstream of
  this. Full write-up + honest caveat in [FACTORY-IMAGE.md](FACTORY-IMAGE.md).
  (Fleet kernels seen: `linux-build-1` factory-3.2.1, `linux-build-2`
  Kitchen/eBay-current, `linux-build-14` eBay's own factory boot.)
- **Same mtdparts** as the units (identical partition offsets), so images flash to
  the same LBAs we've been using.
- Verified factory image md5s (3.2.1): `boot.img fb9b0cf1`, `kernel.img 878097d5`,
  `misc.img 756c8e2b`, `recovery.img eb490ee1`, `system.img 53ff38d0`.
- Unpacked images in scratch `c4img/parts/` (this session); the source zip in
  your own archived copy of that zip is the durable source. RKAF unpacker: manual RKFW/RKAF parse.

**Use for the eBay restore:** flashing this image's `system`+`kernel`+`boot`
(+`recovery`/`misc`) as a *matched set* gives a self-consistent, bootable factory
3.2.1 (no kernel↔system skew — the thing that broke the standalone `3f68fac8`
attempt), after which the 4-line `build.prop` root edit re-roots it. Trade-off: it
downgrades that unit to 3.2.1. A `734549` image would keep it on 4.0.0 and match Kitchen.

## Open questions this inventory should answer

1. Do all three 10"s have different NAND, or is it MICRON-vs-HYNIX in two camps?
2. Does NAND vendor correlate with kernel build (MODVERSIONS or not)? i.e. is
   there genuinely more than one "glassedge10.2.0" kernel in the wild, matched to
   hardware rev?
3. What is each unit's **true stock kernel md5** (before our modifications), and
   which of them has MODVERSIONS (→ can load the camera IPP blob natively)?
4. Is the 7" camera sensor the same NT99141, or different?
5. Which unit is the "possibly fried" one, and how does it fail (loader-visible?
   boots? display?).
