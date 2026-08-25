#!/usr/bin/env bash
#
# flash.sh -- end-to-end: build the MMKeypad app + a custom boot image and
# flash it onto a repurposed Control4 T3 (RK3188) over USB.
#
# The whole "load our stuff" pipeline in one command:
#   1. Enter Rockchip Loader mode      (adb reboot loader, or the recovery button)
#   2. Back up THIS unit's boot         (rkdeveloptool rl -> build/backup-*/boot.orig)
#   3. Build the LVGL app + ramdisk     (zig cross-compile -> rootfs -> cpio)
#   4. Repack a boot.img                 (this unit's OWN kernel + our ramdisk)
#   5. Flash the boot partition          (rkdeveloptool wl 40960 ...)  [gated]
#   6. Reset + verify over the network   (ssh, pgrep mmkeypad, screenshot)
#
# WHY repack against the unit's own kernel: the repo's extracted kernel came
# from a T3-7. Any other T3 (e.g. a 10") is the same RK3188 SoC / ARMv7, so our
# -static userspace runs unchanged, but the panel/DTB lives in the kernel. By
# reusing the unit's own kernel we only ever replace userspace -- panel size,
# touch, and any SoC stepping are carried by the kernel we leave in place.
# (Pass --stock-kernel to force the repo's T3-7 kernel instead.)
#
# The destructive flash (step 5) is gated behind a typed confirmation unless
# --yes is given. Full revert to stock-jailbreak boot: flash.sh --restore.
#
# Boot partition (from reference/t3-control4 README): 24576 sectors @ LBA 40960
# (512B sectors) == 12,582,912 bytes == /dev/mtdblock2 ("boot") on the device.
#
# NETWORK path (--net <ip>): reflash a unit that's already up, no USB. It dumps
# this unit's own boot from mtd2 over SSH (kernel source + first-seen backup),
# builds the image, dd's it to /dev/mtdblock2, read-back-verifies, then reboots.
# CRITICAL: the jailbreak PID-1 does NOT honour the init reboot path -- a plain
# `reboot` silently no-ops, so a freshly-written boot partition keeps running the
# OLD in-RAM kernel/ramdisk and the flash looks like it "didn't take" (this cost
# a full debugging session once). We reboot with `reboot -f` (busybox: direct
# RB_AUTOBOOT syscall, bypasses init) and fall back to sysrq 'b'. Verify then
# waits for the box to actually DROP and confirms a fresh boot (low uptime)
# before trusting it -- so we can't false-verify against the pre-reboot system.
# (The rkdeveloptool USB path resets cleanly via `rk rd` and isn't affected.)
# NOTE: the app-overlay OTA (/data/mmkeypad, respawned by init) needs NO reboot;
# only a full boot.img/init reflash like this one does.
set -euo pipefail

# ---- paths --------------------------------------------------------------
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"                 # firmware-linux-t3/
REPO="$(cd "$ROOT/.." && pwd)"                 # MMKeypad/
RK="$REPO/reference/t3-control4/tools/rkdeveloptool/build/rkDevelopTool_Mac"
STOCK="$REPO/reference/t3-control4/firmware/extracted"
MKBOOT="$ROOT/tools/mkbootimg.py"
APP_SRC="$ROOT/lvgl-app/build/mmk-app"
APP_DST="$ROOT/rootfs/usr/bin/mmkeypad"
RAMDISK="$ROOT/build/ramdisk.cpio.gz"
OUT="$ROOT/build/mmkeypad_boot.img"

# boot partition geometry
BOOT_LBA=40960
BOOT_SECTORS=24576

# system partition geometry (same LBAs flash.sh has always used for --backup-system)
SYSTEM_LBA=7618560
SYSTEM_SECTORS=2097152   # 1 GiB

# ---- options ------------------------------------------------------------
IP=""                 # device IP for post-flash verify (optional)
NET_IP=""             # device IP for NETWORK reflash (no USB/loader mode)
SERIAL=""             # adb serial (auto if a single device)
DO_BACKUP=1
STOCK_KERNEL=0
BUILD_ONLY=0
RESTORE=0
RESTORE_IMG=""        # explicit boot image for --restore (skips per-unit lookup)
ASSUME_YES=0
BACKUP_SYSTEM=0
WRITE_SYSTEM=""       # path to a system image to write (USB path only)

usage() {
  cat <<EOF
flash.sh -- build the MMKeypad app + a custom boot image and flash it onto a
repurposed Control4 T3 (RK3188) over USB, end to end:
  loader mode -> back up this unit's boot -> build app+ramdisk ->
  repack boot.img (this unit's OWN kernel) -> flash [gated] -> reset -> verify.

Usage: tools/flash.sh [options]
  --net <addr>      NETWORK reflash over SSH (no USB/loader mode): push the built
                    boot.img, dd it to the boot partition (mtdblock2), read-back
                    verify, then reboot -f (the jailbreak init IGNORES plain
                    reboot, so a full-image reflash otherwise never applies).
                    Uses this unit's own kernel dumped live from mtd2.
  --ip <addr>       Device IP; enables post-flash SSH verify + screenshot.
  --serial <id>     adb serial (default: the sole attached device).
  --stock-kernel    Repack against the repo's T3-7 kernel, not this unit's.
  --no-backup       Skip the per-unit boot backup (NOT recommended).
  --backup-system   Also dump the ~1GB system partition (~70s) before flashing.
  --write-system P  Also write system image P to the system partition (USB path
                    only). Use when a unit's /system doesn't match the universal
                    kernel we flash: /system carries the wifi/gadget kernel
                    modules, so version skew silently kills wlan0, rndis and adb
                    (the eBay-10 failure). P is typically a known-good dump, e.g.
                    build/backup-10-*/system.img.
  --build-only      Build the boot image only; no device I/O (implies stock kernel).
  --restore         Flash this unit's backed-up boot.orig (revert) and exit.
                    Over --net this dd's boot.orig back to mtdblock2 + reboot -f.
                    Refuses if THIS unit has no backup (another unit's kernel
                    carries the wrong panel/DTB); name one with --restore-image.
  --restore-image P Restore from an explicit boot image P (implies --restore).
                    P must hold this unit's own kernel.
  --yes             Skip the typed pre-flash confirmation.
  -h, --help        This help.

USB (loader-mode) path uses rkdeveloptool; --net uses only SSH (root@<addr>,
~/.ssh/id_rsa) and works on a unit that's already up on the network.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --net)           NET_IP="$2"; shift 2;;
    --ip)            IP="$2"; shift 2;;
    --serial)        SERIAL="$2"; shift 2;;
    --stock-kernel)  STOCK_KERNEL=1; shift;;
    --kernel-image)  KERNEL_OVERRIDE="$2"; shift 2;;
    --no-backup)     DO_BACKUP=0; shift;;
    --backup-system) BACKUP_SYSTEM=1; shift;;
    --write-system)  WRITE_SYSTEM="$2"; shift 2;;
    --build-only)    BUILD_ONLY=1; STOCK_KERNEL=1; shift;;
    --restore)       RESTORE=1; shift;;
    --restore-image) RESTORE_IMG="$2"; RESTORE=1; shift 2;;
    --yes)           ASSUME_YES=1; shift;;
    -h|--help)       usage; exit 0;;
    *) echo "unknown option: $1" >&2; usage; exit 2;;
  esac
done

# ---- helpers ------------------------------------------------------------
c_g=$'\033[32m'; c_y=$'\033[33m'; c_r=$'\033[31m'; c_b=$'\033[1m'; c_0=$'\033[0m'
step() { echo; echo "${c_b}==> $*${c_0}"; }
info() { echo "    $*"; }
ok()   { echo "${c_g}    OK: $*${c_0}"; }
warn() { echo "${c_y}    !! $*${c_0}"; }
die()  { echo "${c_r}ERROR: $*${c_0}" >&2; exit 1; }

need() { command -v "$1" >/dev/null 2>&1 || die "missing tool: $1"; }

# The T3 app is a zig cross-build that pulls LVGL *source* from the ESP-IDF
# managed-components tree. That dir is normally populated by `idf.py build`;
# if it's been cleaned, fetch the locked LVGL version (9.3.0) so this script is
# self-contained and doesn't require a full ESP-IDF toolchain just to build the
# T3 image. (A later `idf.py` will reconcile it against the registry copy.)
LVGL_DIR="$REPO/firmware-idf/managed_components/lvgl__lvgl"
LVGL_TAG="v9.3.0"
ensure_lvgl() {
  [[ -f "$LVGL_DIR/lvgl.h" ]] && return
  step "LVGL source missing -- fetching $LVGL_TAG into managed_components/"
  need git
  mkdir -p "$(dirname "$LVGL_DIR")"
  git clone --depth 1 --branch "$LVGL_TAG" https://github.com/lvgl/lvgl.git "$LVGL_DIR"
  [[ -f "$LVGL_DIR/lvgl.h" ]] || die "LVGL fetch failed ($LVGL_DIR/lvgl.h absent)"
  ok "LVGL $LVGL_TAG ready"
}

rk() { "$RK" "$@"; }
in_loader() { "$RK" ld 2>/dev/null | grep -qi loader; }

# ---- network (SSH) reflash helpers -------------------------------------
# The jailbreak PID-1 does NOT honour the init reboot path, so `reboot`,
# `ssh root@t3 reboot`, etc. silently no-op and a freshly-flashed boot
# partition never actually boots. reboot -f (busybox: direct RB_AUTOBOOT
# syscall) bypasses init and works; sysrq 'b' is the fallback.
NKEY="$HOME/.ssh/id_rsa"
nssh() {
  ssh -i "$NKEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
      -o ConnectTimeout=6 "root@$NET_IP" "$@"
}
net_up() { nssh true 2>/dev/null; }

# Force a real reboot over SSH (backgrounded so ssh returns before the link drops).
net_reboot() {
  step "Rebooting the device (reboot -f -- plain 'reboot' is a no-op on this init)"
  nssh 'sync; sync; (sleep 1; reboot -f || echo b > /proc/sysrq-trigger) >/dev/null 2>&1 &' \
    2>/dev/null || true
  ok "reboot dispatched"
}

# Push an image, verify md5 on-device, dd to a partition, read back + verify.
# $1 = local image (partition-sized), $2 = /dev/mtdblockN target.
net_write_partition() {
  local img="$1" dev="$2"
  local sz md5 pages rbmd5
  sz="$(wc -c < "$img")"; md5="$(md5 -q "$img")"; pages=$(( (sz + 65535) / 65536 ))
  info "image: $img ($sz bytes, md5 $md5) -> $dev"
  cat "$img" | nssh "cat > /data/boot.new && [ \"\$(md5sum /data/boot.new | cut -d' ' -f1)\" = \"$md5\" ] && echo PUSH_OK" \
    2>/dev/null | grep -q PUSH_OK || die "push to device failed or md5 mismatch (nothing written to NAND)"
  ok "pushed + md5 verified in /data/boot.new"
  step "Writing $dev (read-back verified)"
  rbmd5="$(nssh "dd if=/data/boot.new of=$dev bs=65536 2>/dev/null; sync; dd if=$dev bs=65536 count=$pages 2>/dev/null | md5sum | cut -d' ' -f1" 2>/dev/null)"
  [[ "$rbmd5" == "$md5" ]] || die "NAND read-back md5 mismatch (got $rbmd5, want $md5) -- flash may be bad"
  nssh 'rm -f /data/boot.new' 2>/dev/null || true
  ok "$dev written + verified ($rbmd5)"
}

adb_serial() {
  [[ -n "$SERIAL" ]] && { echo "$SERIAL"; return; }
  local s
  s="$(adb devices 2>/dev/null | awk 'NR>1 && $2!="" {print $1}' | head -1)"
  echo "$s"
}

# Poll until the device shows up in Loader mode (or time out).
wait_loader() {
  local secs="${1:-40}"
  for ((i=0; i<secs; i++)); do
    in_loader && return 0
    sleep 1
  done
  return 1
}

enter_loader() {
  step "Entering Rockchip Loader mode"
  if in_loader; then ok "already in Loader mode"; return; fi

  local s; s="$(adb_serial)"
  if [[ -n "$s" ]]; then
    info "adb sees '$s' -- issuing 'adb reboot loader'"
    adb -s "$s" reboot loader 2>/dev/null || warn "adb reboot loader failed (recovery adbd may not allow it)"
    if wait_loader 40; then ok "in Loader mode"; return; fi
  fi

  warn "device is not in Loader mode and adb couldn't switch it."
  cat <<EOF
    Force it by hand:
      1. Unplug USB.
      2. Hold the UNLABELED recovery button on the back.
      3. While holding it, plug USB back in; keep holding ~2s.
    The device should enumerate as USB 2207:310b (Loader).
EOF
  read -r -p "    Press Enter once you've done that (or Ctrl-C to abort)... " _
  wait_loader 40 || die "still not in Loader mode ('$RK ld' shows nothing)"
  ok "in Loader mode"
}

# ---- preflight ----------------------------------------------------------
need python3
need zig
# rkdeveloptool + adb are only needed for the USB (loader-mode) path.
if [[ -z "$NET_IP" ]]; then
  [[ -x "$RK" ]] || die "rkdeveloptool not found/executable: $RK"
  if [[ $BUILD_ONLY -eq 0 ]]; then need adb; fi
fi
# The stock extraction is only used when repacking against the repo's T3-7
# kernel (--stock-kernel, implied by --build-only). The default USB path and the
# whole --net path repack against THIS unit's own kernel (dumped from mtd2 / the
# loader backup), so they don't need $STOCK at all — don't demand it there.
if [[ $STOCK_KERNEL -eq 1 ]]; then
  [[ -f "$STOCK/kernel.img" && -f "$STOCK/boot.orig" ]] || die "missing stock kernel/boot in $STOCK"
fi

if [[ -n "$WRITE_SYSTEM" ]]; then
  [[ -z "$NET_IP" ]] || die "--write-system is USB/loader-mode only (1GB over ssh-dd is untested)"
  [[ -f "$WRITE_SYSTEM" ]] || die "system image not found: $WRITE_SYSTEM"
  sys_bytes=$(stat -f%z "$WRITE_SYSTEM" 2>/dev/null || stat -c%s "$WRITE_SYSTEM")
  (( sys_bytes <= SYSTEM_SECTORS * 512 )) || die "system image larger than the partition ($sys_bytes > $((SYSTEM_SECTORS*512)))"
fi

if [[ -n "$NET_IP" ]]; then
  need ssh
  [[ -f "$NKEY" ]] || die "ssh key not found: $NKEY"
  net_up || die "cannot reach root@$NET_IP over SSH (is the T3 up on the network?)"
fi

# Per-unit backup dir. USB path keys on adb serial; net path keys on the eth MAC
# (a stable per-unit id) so multiple units don't clobber each other's backups.
if [[ -n "$NET_IP" ]]; then
  UNIT="$(nssh 'cat /sys/class/net/eth0/address 2>/dev/null | tr -d :' 2>/dev/null || true)"
  UNIT="net-${UNIT:-$(echo "$NET_IP" | tr . -)}"
else
  UNIT="$(adb_serial)"; UNIT="${UNIT:-unit}"
fi
BK="$ROOT/build/backup-$UNIT"

# ---- restore path -------------------------------------------------------
if [[ $RESTORE -eq 1 ]]; then
  # The boot partition carries the KERNEL, and the kernel carries the panel/DTB.
  # Restoring another unit's boot.orig onto this unit yields a wrong-panel boot
  # (e.g. a 10" kernel on a 7"). So we NEVER guess across units: it's this unit's
  # own backup, or an image the operator named explicitly with --restore-image.
  if [[ -n "$RESTORE_IMG" ]]; then
    BOOT_BAK="$RESTORE_IMG"
    [[ -f "$BOOT_BAK" ]] || die "--restore-image not found: $BOOT_BAK"
    warn "restoring an EXPLICIT image (not this unit's backup): $BOOT_BAK"
    warn "it must contain THIS unit's kernel -- a mismatched panel/DTB won't boot usably."
  else
    BOOT_BAK="$BK/boot.orig"
    [[ -f "$BOOT_BAK" ]] || die "no backup for this unit at $BOOT_BAK
Refusing to fall back to another unit's boot.orig (wrong kernel/panel). Available:
$(ls -1 "$ROOT"/build/backup-*/boot.orig 2>/dev/null | sed 's/^/  /')
If one of the above IS this unit's, pass it: tools/flash.sh --restore-image <path>"
  fi
  if [[ -n "$NET_IP" ]]; then
    step "Restoring backed-up boot over the network from $BOOT_BAK"
    net_write_partition "$BOOT_BAK" /dev/mtdblock2
    net_reboot
    ok "restored + reboot dispatched. Device should boot the backed-up image."
    exit 0
  fi
  enter_loader
  step "Restoring stock boot from $BOOT_BAK"
  rk wl "$BOOT_LBA" "$BOOT_BAK"
  rk rd
  ok "restored + reset. Device should boot the stock Control4 image."
  exit 0
fi

# KERNEL_SRC_BOOT: the boot.img we unpack THIS unit's kernel from when
# repacking (net mode dumps it live from mtd2; USB mode uses the loader backup).
KERNEL_SRC_BOOT="$BK/boot.orig"

# ---- 1+2 (network): dump this unit's current boot from mtd2 -------------
if [[ -n "$NET_IP" && $BUILD_ONLY -eq 0 ]]; then
  step "Reading device identity (network)"
  info "unit: $UNIT   ($(nssh 'uname -sr' 2>/dev/null || echo '?'))"
  mkdir -p "$BK"
  # Live dump of the current boot partition -> used for the kernel repack AND,
  # the first time we see this unit, kept as its boot backup. mtd2 is 12 MiB.
  CUR="$BK/boot.current"
  step "Dumping this unit's boot partition (mtd2) over SSH -> $CUR"
  nssh 'dd if=/dev/mtdblock2 bs=65536 2>/dev/null' > "$CUR" 2>/dev/null
  [[ -s "$CUR" ]] || die "failed to dump mtdblock2 over SSH"
  ok "dumped $(du -h "$CUR" | cut -f1)"
  if [[ $DO_BACKUP -eq 1 && ! -f "$BK/boot.orig" ]]; then
    cp "$CUR" "$BK/boot.orig"
    ok "kept first-seen boot as backup: $BK/boot.orig (revert: flash.sh --net $NET_IP --restore)"
  fi
  KERNEL_SRC_BOOT="$CUR"
fi

# ---- 1. loader mode + 2. backup (USB) ----------------------------------
if [[ -z "$NET_IP" && $BUILD_ONLY -eq 0 ]]; then
  enter_loader

  step "Reading device identity"
  rk rci 2>/dev/null || true
  rk rfi 2>/dev/null || true

  if [[ $DO_BACKUP -eq 1 ]]; then
    step "Backing up this unit's boot partition -> $BK/"
    mkdir -p "$BK"
    if [[ -f "$BK/boot.orig" ]]; then
      ok "boot.orig already backed up ($(du -h "$BK/boot.orig" | cut -f1)); keeping it"
    else
      rk rl "$BOOT_LBA" "$BOOT_SECTORS" "$BK/boot.orig"
      ok "saved $BK/boot.orig"
    fi
    if [[ $BACKUP_SYSTEM -eq 1 && ! -f "$BK/system.bin" ]]; then
      info "dumping system (0x200000 @ 0x744000, ~70s)..."
      rk rl 7618560 2097152 "$BK/system.bin"
      ok "saved $BK/system.bin"
    fi
  else
    warn "skipping backup (--no-backup): you will have NO restore path for boot."
  fi
fi

# ---- 3. build app + ramdisk --------------------------------------------
ensure_lvgl
step "Building the LVGL app (zig cross-compile, arm-linux-musleabihf)"
make -C "$ROOT/lvgl-app"
[[ -f "$APP_SRC" ]] || die "app build produced no $APP_SRC"
# rootfs/usr/bin holds only the (gitignored) app binary, so git never creates the
# dir on a fresh checkout — make it before staging or the cp dies.
mkdir -p "$(dirname "$APP_DST")"
cp "$APP_SRC" "$APP_DST"
ok "staged app -> rootfs/usr/bin/mmkeypad ($(du -h "$APP_DST" | cut -f1))"

step "Building init + ramdisk (busybox + dropbear + app)"
make -C "$ROOT" ramdisk
[[ -f "$RAMDISK" ]] || die "ramdisk build produced no $RAMDISK"
ok "ramdisk ready"

# ---- 4. pick kernel + pack boot image ----------------------------------
if [[ -n "${KERNEL_OVERRIDE:-}" ]]; then
  # Explicit kernel image (e.g. the TRUE 10" stock kernel that has MODVERSIONS
  # and can load the vendor graphics/media blobs -- the repo's older kernel
  # can't). Header comes from this unit's own boot (identical load addrs; RK
  # cmdline lives in the parameter partition, not the boot.img).
  [[ -f "$KERNEL_OVERRIDE" ]] || die "--kernel-image not found: $KERNEL_OVERRIDE"
  KERNEL="$KERNEL_OVERRIDE"
  HEADER="$KERNEL_SRC_BOOT"
  [[ -f "$HEADER" ]] || HEADER="$STOCK/boot.orig"
  step "Packing boot image with EXPLICIT kernel: $(basename "$KERNEL_OVERRIDE")"
elif [[ $STOCK_KERNEL -eq 1 ]]; then
  KERNEL="$STOCK/kernel.img"
  HEADER="$STOCK/boot.orig"
  step "Packing boot image with the repo (T3-7) kernel"
else
  BOOT_BAK="$KERNEL_SRC_BOOT"
  [[ -f "$BOOT_BAK" ]] || die "no per-unit boot image at $BOOT_BAK; run without --no-backup, or pass --stock-kernel"
  step "Unpacking this unit's kernel from its own boot image ($(basename "$BOOT_BAK"))"
  UNP="$ROOT/build/unit-kernel"
  mkdir -p "$UNP"
  python3 "$MKBOOT" unpack "$BOOT_BAK" "$UNP"
  KERNEL="$UNP/kernel.img"
  HEADER="$BOOT_BAK"
  ok "using this unit's own kernel ($(du -h "$KERNEL" | cut -f1))"
fi

python3 "$MKBOOT" pack --kernel "$KERNEL" --ramdisk "$RAMDISK" --header-from "$HEADER" -o "$OUT"
ok "boot image: $OUT ($(du -h "$OUT" | cut -f1))"

if [[ $BUILD_ONLY -eq 1 ]]; then
  step "Build-only: not touching the device."
  info "flash it later with: tools/flash.sh   (or re-run without --build-only)"
  exit 0
fi

# ---- 5. flash (gated) ---------------------------------------------------
if [[ -z "$NET_IP" ]]; then
  in_loader || { warn "device left Loader mode during build; re-entering"; enter_loader; }
fi

if [[ $ASSUME_YES -eq 0 ]]; then
  step "About to OVERWRITE the boot partition"
  cat <<EOF
    device unit : $UNIT
    writing     : $OUT
    to          : $( [[ -n "$NET_IP" ]] && echo "boot @ /dev/mtdblock2 over SSH (root@$NET_IP)" || echo "boot @ LBA $BOOT_LBA ($BOOT_SECTORS sectors)" )$( [[ -n "$WRITE_SYSTEM" ]] && echo "
    system      : $WRITE_SYSTEM -> LBA $SYSTEM_LBA" )
    kernel      : $( [[ $STOCK_KERNEL -eq 1 ]] && echo "repo T3-7 kernel" || echo "this unit's own kernel" )
    restore     : tools/flash.sh $( [[ -n "$NET_IP" ]] && echo "--net $NET_IP " )--restore   (writes back $BK/boot.orig)
EOF
  read -r -p "    Type 'flash' to proceed: " ans
  [[ "$ans" == "flash" ]] || die "aborted by user"
fi

if [[ -n "$NET_IP" ]]; then
  step "Flashing boot partition over the network"
  net_write_partition "$OUT" /dev/mtdblock2
  net_reboot
  # Auto-verify against the same unit unless a different --ip was given.
  [[ -z "$IP" ]] && IP="$NET_IP"
else
  if [[ -n "$WRITE_SYSTEM" ]]; then
    step "Flashing system partition ($(du -h "$WRITE_SYSTEM" | cut -f1) -- takes a while over loader USB)"
    rk wl "$SYSTEM_LBA" "$WRITE_SYSTEM"
    ok "system written"
  fi

  step "Flashing boot partition"
  rk wl "$BOOT_LBA" "$OUT"
  ok "written"

  step "Resetting device"
  rk rd
  ok "reset issued"
fi

# ---- 6. verify ----------------------------------------------------------
if [[ -n "$IP" ]]; then
  KEY="$HOME/.ssh/id_rsa"
  SSH=(ssh -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5)

  # NET mode: SSH is still up on the OLD kernel for a moment after reboot -f, so
  # wait for the box to actually DROP before we trust "it's up" -- otherwise we'd
  # false-verify against the pre-reboot system (the exact bug this mode fixes).
  if [[ -n "$NET_IP" ]]; then
    step "Waiting for the device to drop (confirming the reboot took)"
    dropped=0
    for ((i=0; i<30; i++)); do
      "${SSH[@]}" "root@$IP" true 2>/dev/null || { dropped=1; break; }
      sleep 2
    done
    [[ $dropped -eq 1 ]] && ok "device dropped" \
      || warn "device never dropped in 60s -- reboot -f may not have taken; verify may be stale"
  fi

  step "Waiting for the device on the network ($IP)"
  up=0
  for ((i=0; i<60; i++)); do
    if "${SSH[@]}" "root@$IP" true 2>/dev/null; then up=1; break; fi
    sleep 2
  done
  if [[ $up -eq 1 ]]; then
    ok "SSH is up"
    # Prove it's a FRESH boot (low uptime), not the old system still running.
    upt="$("${SSH[@]}" "root@$IP" 'cut -d. -f1 /proc/uptime' 2>/dev/null || echo '?')"
    if [[ "$upt" =~ ^[0-9]+$ && "$upt" -lt 300 ]]; then
      ok "fresh boot confirmed (uptime ${upt}s)"
    else
      warn "uptime is ${upt}s -- NOT a fresh boot; the flashed image may not have booted"
    fi
    if "${SSH[@]}" "root@$IP" 'pgrep -f mmkeypad >/dev/null'; then
      ok "mmkeypad is running"
    else
      warn "mmkeypad not detected via pgrep -- check 'cat /data/mmkinit-boot.log'"
    fi
    "${SSH[@]}" "root@$IP" 'grep -q "OTA overlay" /data/mmkinit-boot.log 2>/dev/null' \
      && ok "OTA overlay active (running /data/mmkeypad)" || true
    step "Grabbing a screenshot"
    if bash "$ROOT/tools/screenshot.sh" "$IP" "$ROOT/build/t3-boot-$UNIT.png"; then
      ok "screenshot -> build/t3-boot-$UNIT.png"
    fi
  else
    warn "no SSH after 2 min. If the display is up but no network, check ethernet."
    info "recovery: tools/flash.sh $( [[ -n "$NET_IP" ]] && echo "--net $NET_IP " )--restore"
  fi
else
  step "Done -- no --ip given, so no auto-verify."
  info "Watch it boot; once on the network: ssh -i ~/.ssh/id_rsa root@<dhcp-ip>"
  info "Boot log: ssh root@<ip> cat /data/mmkinit-boot.log"
  info "Screenshot: tools/screenshot.sh <ip>"
  info "Revert to stock: tools/flash.sh --restore"
fi
