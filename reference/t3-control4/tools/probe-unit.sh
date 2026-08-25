#!/usr/bin/env bash
# Fingerprint one T3 unit for the per-unit comparison inventory (UNIT-INVENTORY.md).
#
# Collects a consistent set of hardware + firmware data points so units can be
# compared conclusively. Works against whatever channel reaches the unit:
#
#   probe-unit.sh adb                 # rooted stock Android (adb root shell)
#   probe-unit.sh ssh   root@<ip>     # our custom Linux over the network
#   probe-unit.sh serial /dev/cu.usbmodem*   # our custom Linux over USB console
#
# For the DEFINITIVE NAND chip, also run rkdeveloptool rfi in loader mode (the
# live /proc/mtd + dmesg agree, but the loader is the ground truth).
#
# Output is a labeled block; paste it into UNIT-INVENTORY.md under the unit.
set -uo pipefail
MODE="${1:?usage: probe-unit.sh <adb|ssh|serial> [target]}"; TARGET="${2:-}"

# run a shell snippet on the unit, whatever the channel
runon() {
  local script="$1"
  case "$MODE" in
    adb)    adb shell "$script" 2>&1 ;;
    ssh)    ssh -o ConnectTimeout=6 -o StrictHostKeyChecking=accept-new "$TARGET" "$script" 2>&1 \
              | grep -viE "post-quantum|store now|vulnerable|pq.html|WARNING: connection|upgraded" ;;
    serial) printf '%s\n' "$script" > "$TARGET"; sleep 3; timeout 4 cat "$TARGET" 2>/dev/null ;;
    *) echo "unknown mode: $MODE" >&2; exit 2 ;;
  esac
}

# One combined remote script — labels each field so the output self-documents.
read -r -d '' PROBE <<'EOS'
echo "== identity =="
cat /proc/version
getprop ro.product.model 2>/dev/null; getprop ro.build.date 2>/dev/null; getprop ro.build.version.incremental 2>/dev/null
getprop ro.serialno 2>/dev/null
ifconfig eth0 2>/dev/null | grep -iE "hwaddr|ether" | head -1
echo "== ram =="
grep MemTotal /proc/meminfo
echo "== nand (mtd) =="
cat /proc/mtd
dmesg | grep -iE "nand.*id|flash id|manufacture|hynix|micron|samsung|toshiba|sandisk" | tail -4
echo "== modversions? =="
zcat /proc/config.gz 2>/dev/null | grep -iE "CONFIG_MODVERSIONS|CONFIG_MODULE_FORCE" || echo "no /proc/config.gz"
echo "== loaded modules =="
cat /proc/modules 2>/dev/null | awk '{print $1}'
echo "== module vermagics (/system/lib/modules) =="
for m in rk29-ipp mali ump vpu_service rk30_mirroring rkwifi.oob; do
  f=/system/lib/modules/$m.ko; [ -f "$f" ] && printf "%-14s " "$m" && strings "$f" | grep -m1 vermagic
done
echo "== wifi chip =="
cat /sys/class/rkwifi/chip 2>&1
dmesg | grep -iE "ap6|rk90|bcm4|rtl8|esp8|mt76|wlan.*chip" | tail -3
echo "== touch =="
cat /proc/bus/input/devices 2>/dev/null | grep -iE "Name="
echo "== camera =="
ls -la /dev/video* 2>&1
dmesg | grep -iE "nt99|ov2|gc[0-9]|camera version|sensor_probe|rk_cam_cif" | tail -4
echo "== display/panel =="
cat /sys/class/graphics/fb0/virtual_size 2>&1; cat /sys/class/graphics/fb0/bits_per_pixel 2>&1; cat /sys/class/graphics/fb0/modes 2>&1
dmesg | grep -iE "lcdc|dclk|screen" | tail -3
echo "== efuse (chip uid) =="
cat /sys/devices/system/cpu/efuse_val 2>&1
echo "== i2c map =="
for b in 0 1 2 3 4; do echo "i2c-$b:"; i2cdetect -y $b 2>/dev/null | tail -8; done
echo "== end =="
EOS

echo "############ PROBE $(date -u +%FT%TZ) via $MODE $TARGET ############"
runon "$PROBE"
