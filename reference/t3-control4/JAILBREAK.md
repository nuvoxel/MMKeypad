# Control4 T3 — persistent root via unsigned `/system` reflash

Field-proven on a **Control4 T3-7** (RK3188, Android 4.4.2, C4 OS 4.0.0),
2026-07-04. Yields a **persistent, authorized root adb shell** on an otherwise
locked-down C4 touchscreen, **without modifying the (integrity-checked) `boot`
partition** and while **SELinux stays Enforcing**. Fully reversible.

> All writes here are to `/system` only; `boot`/`recovery`/loader are never
> touched. Restore = reflash the pristine `10_system.bin` backup. See
> [`README.md`](README.md) for the hardware teardown and Loader-mode access.

> **Two jailbreak paths exist — pick by goal:**
> - **(A) This doc — unsigned `/system` reflash → rooted stock Android.** Keeps
>   Control4's OS; good for inspecting the stock stack. Leaves `boot` untouched.
> - **(B) Repacked `boot.img` → fully custom Linux (RECOMMENDED, what we ship).**
>   CONFIRMED on both 7" and 10". Replaces only the initramfs, keeps the unit's own
>   kernel, boots our LVGL app on a musl userspace. See README →
>   [Custom-Linux post-flash access](README.md#custom-linux--post-flash-access-2026-07-18)
>   and [`firmware-linux-t3/`](../../firmware-linux-t3/). Both paths use the same
>   Loader-mode entry + full backup below.

## TL;DR
1. Get into Rockchip **Loader mode** (hold the unlabeled recovery button while
   plugging USB) and **back up every partition** with `rkdeveloptool rl`.
2. Edit **`/system/build.prop`** offline (in the ext4 dump) to enable Rockchip's
   root adbd, then reflash just the `system` partition.
3. Reboot → the device comes up normally into the C4 UI **and** exposes a
   **`uid=0(root)` adb shell over USB**.

The whole trick: **`/system` is a plain, mutable ext4 with NO dm-verity**, so an
unsigned/modified `/system` boots fine. The signed/verified thing on this device
is `boot`, which we leave alone.

## Why it works (security model, as measured)
- **`/system` is NOT verity-protected.** The dumped `system` partition is a
  normal read-write ext4 (mountable, ~358 MB free, files carry SELinux labels but
  there is no verity hash tree). A modified `/system` reflashed byte-for-byte
  boots without complaint. **This is the jailbreak vector.**
- **`boot` IS integrity-checked.** Flipping a single (functionally-irrelevant)
  byte in the `boot` image → on reboot the bootloader **refuses it and falls back
  to Android recovery** ("No command"). Restoring the exact original boots
  normally. So boot images are validated; `/system` images are not.
- **No OEM secure-boot key appears to be fused.** From root:
  `cat /sys/devices/system/cpu/efuse_val` →
  `524b1388 51fae107 04040203 02230113 62a61800 00623002 00000000 00000000`
  — opens with `52 4b` = **"RK"** + chip marker, then a per-unit chip UID, and the
  **key-hash region is all zeros**. No burned RSA public-key hash ⇒ consistent
  with **OEM secure boot disabled**. That implies the `boot` rejection above is a
  **CRC/integrity** check, *not* an RSA signature — i.e. a *properly repacked*
  boot image (valid RK CRC) would boot. **CONFIRMED 2026-07-18** on both the 7"
  and the 10": a `boot.img` repacked from the unit's own kernel + our own
  initramfs boots a fully custom Linux userspace. That is now our primary path
  (see below); the `/system`-root method here is the alternative that keeps stock
  Android.
- **SELinux is Enforcing** (`getenforce` = Enforcing) yet the adb shell runs as
  **`uid=0(root) context=u:r:shell:s0`** — the shell domain is permitted root on
  this build. The catch this creates: any file you write into `/system` **must
  carry the right SELinux xattr** or Enforcing init won't read it (see step 3).

## The property that actually grants root (measured, not assumed)
`ro.*` properties are frozen to whatever the **boot ramdisk `default.prop`** set
first, and `/boot` is exactly what we can't modify — so **`ro.debuggable=1` in
`build.prop` is IGNORED** (`getprop ro.debuggable` stays `0` at runtime). Root
comes instead from Rockchip's own mutable `sys.` toggle:

| build.prop change | effect |
|---|---|
| `sys.rkadb.root=1` | **the key** — Rockchip's adbd runs as **root** |
| `ro.adb.secure=0` | adb is **pre-authorized** (no RSA host-key prompt) |
| `persist.sys.usb.config=mass_storage` → `adb` | exposes the **adb** USB interface (was mass-storage; `/data` wiped so this default is what boot uses) |
| `ro.debuggable=1` | *no-op here* (ramdisk `default.prop` wins) — harmless to leave |

## Prerequisites
- `rkdeveloptool` (built from source; macOS needs `make CXXFLAGS="-Wno-error"`).
- `e2fsprogs` (`debugfs`, `e2fsck`) — on macOS: `brew install e2fsprogs`
  (`/opt/homebrew/opt/e2fsprogs/sbin/`).
- `adb` (`brew install android-platform-tools`).
- A **full partition backup** first (non-negotiable — this is the restore path).

## Method

### 1. Loader mode + full backup
Hold the unlabeled **recovery** button while plugging USB → `rkdeveloptool ld`
shows `2207:310b Loader`. Then dump (at least) the `system` partition; ideally
everything (see README partition map). LBAs are 512-byte sectors:
```
# system = 0x200000 sectors @ 0x744000  → decimal: 2097152 @ 7618560
rkdeveloptool rl 7618560 2097152 10_system.bin     # ~70s, ~16 MB/s
# boot   = 0x6000   sectors @ 0xa000    → decimal:   24576 @   40960  (keep as restore-only)
rkdeveloptool rl 40960 24576 boot.orig
```

### 2. Edit `/system/build.prop` in the ext4 dump (offline, on a COPY)
```
DBG=/opt/homebrew/opt/e2fsprogs/sbin/debugfs
cp 10_system.bin system_mod.bin                     # never edit the pristine backup
$DBG -R "dump /build.prop /dev/stdout" system_mod.bin > build.prop.orig
sed -e 's/^persist\.sys\.usb\.config=mass_storage/persist.sys.usb.config=adb/' \
    -e 's/^ro\.adb\.secure *=.*/ro.adb.secure=0/' \
    -e 's/^sys\.rkadb\.root=0/sys.rkadb.root=1/' \
    build.prop.orig > build.prop.mod
grep -q '^ro.debuggable=' build.prop.mod || printf 'ro.debuggable=1\n' >> build.prop.mod
```

### 3. Write it back — and RESTORE THE SELINUX XATTR (critical)
`debugfs write` drops the `security.selinux` xattr; under Enforcing SELinux,
init then can't read `build.prop` and the device won't boot right. Re-set it to
the same context the original had (`u:object_r:system_file:s0`, **26 bytes incl.
trailing NUL**):
```
printf 'u:object_r:system_file:s0\000' > ctx.bin
$DBG -w -R "rm /build.prop"                       system_mod.bin
$DBG -w -R "write build.prop.mod /build.prop"     system_mod.bin
$DBG -w -R "ea_set -f ctx.bin /build.prop security.selinux" system_mod.bin
# verify + fsck before flashing
$DBG -R "ea_get /build.prop security.selinux" system_mod.bin   # expect u:object_r:system_file:s0
/opt/homebrew/opt/e2fsprogs/sbin/e2fsck -fn system_mod.bin     # must be clean
```

### 4. Reflash `/system` only, keep `boot` untouched
```
# (in Loader mode) sanity-check target holds ext4 (superblock magic 53 EF @ 0x438), then:
rkdeveloptool wl 7618560 system_mod.bin            # ~70s
rkdeveloptool rl 7618560 2097152 system_verify.bin # optional full read-back
# md5 system_verify.bin == md5 system_mod.bin
rkdeveloptool rd                                   # normal reboot
```

### 5. Result
Device boots the C4 UI normally (it will sit on "Connecting to Director" if no C4
Director is on the LAN — cosmetic). Over USB you now have:
```
$ adb devices          # 000fffxxxxxx  device      (000fff = Control4 MAC OUI)
$ adb shell id         # uid=0(root) gid=0(root) context=u:r:shell:s0
```

## What this unlocks
- `adb install` any (KitKat/API-19, armeabi-v7a) APK — e.g. a standard launcher
  to escape the C4 kiosk (the sole registered HOME is
  `com.control4.android.launcher/.ui.activity.MainActivity`; no AOSP launcher is
  present, so one must be added).
- Full read of the C4 client stack for protocol study (`/system/app/*.apk`,
  `/system/control4` agent) — how the official touchscreen talks to the Director,
  useful for the DriverWorks driver + HA integration.
- Live inspection: props, efuse, dmesg, partitions.

## De-Control4'ing: kill the reboot watchdog + swap the launcher
Once rooted, replacing the C4 launcher fails at first because C4 **reboots the
device** whenever its launcher loses home focus (or is force-stopped). Mechanism
(measured): it is **NOT** the `/system/control4` agent or crond (those only run
`logrotate`). It's the **app layer** — `com.control4.deviceadministrator` is an
**active device admin running as uid=1000 (system)** (policies wipe-data,
disable-camera) alongside `com.control4.upmand`; that layer reboots on launcher
loss. Kill it live (reversible, no reflash):
```
adb root
adb shell pm disable com.control4.upmand
adb shell pm disable com.control4.deviceadministrator
adb shell pm disable com.control4.phoenix
adb shell pm disable com.control4.sddp
# verify the watchdog is dead: force-stopping the launcher no longer reboots
adb shell am force-stop com.control4.android.launcher   # uptime keeps climbing = dead
# then install any KitKat launcher + make it sole home:
adb install Launcher.apk                                 # e.g. Nova 5.5.4 (minAPI16)
adb shell pm disable com.control4.android.launcher
adb shell am start -a android.intent.action.MAIN -c android.intent.category.HOME
# panel may be asleep — wake + hold it on:
adb shell 'input keyevent 26; svc power stayon true'
```
Result: a de-Control4'd, rooted Android 4.4.2 panel booting to a stock launcher,
stable (no reboot loop). Fully reversible: `pm enable` the four packages + the
launcher. NOTE: no `dpm` shell cmd on KitKat (API 19) to remove the device admin,
but disabling the package neuters it (`/data` was wiped so no admin is re-armed).
Verified on hardware 2026-07-04 (Nova 5.5.4 as home). **The HA companion app will
NOT install here** — it needs Android 6.0 / API 23 (minimal flavor: API 21); this
is API 19. A real HA/modern-app panel needs the Linux route (see README repurpose).

## Reversibility / safety
- **`boot` and the loader are never written** — the device always retains its
  original, signed boot chain. Recovery-button + USB re-enters Loader mode any
  time.
- To fully revert: `rkdeveloptool wl 7618560 10_system.bin` (pristine dump) →
  reboot. Back to bone-stock.
- Everything above was performed read-mostly with a verified backup in hand; the
  T3 boots Control4 normally throughout.

## Open items
- ~~**Definitive lock verdict:** repack a valid-CRC, non-C4 `boot` and flash it.~~
  **RESOLVED 2026-07-18 — boots.** A repacked `boot.img` (unit's own kernel + our
  initramfs) runs custom Linux on both 7" and 10", so `boot` uses a CRC, not RSA
  secure boot — the device is effectively unlocked (custom kernels/initramfs OK).
- **Launcher swap** not yet performed (root obtained; `adb install` + set-home is
  the remaining step). N/A on the custom-Linux path (we replace userspace entirely).

## Artifacts (local only, git-ignored)
`system_mod.bin` (patched), `build.prop.orig/.mod`, `ctx.bin`, partition dumps —
in the working dir alongside the README's firmware dumps.
