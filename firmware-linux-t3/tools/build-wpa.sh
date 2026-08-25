#!/usr/bin/env bash
# Cross-build wpa_supplicant + wpa_cli for the T3 (armv7 musl, static) with the
# nl80211 backend + libnl-tiny + wpa's INTERNAL crypto/TLS (no OpenSSL — WPA2-PSK
# only needs the built-in crypto). Fetches the source trees into the (gitignored)
# thirdparty/ dir on first run, then builds. Produces:
#   thirdparty/libnl-tiny/libnl-tiny.a
#   thirdparty/wpa_supplicant-2.10/wpa_supplicant/{wpa_supplicant,wpa_cli}  (stripped ARM ELF)
#
# `make ramdisk` (the wifi-tools target) runs this if the binaries are missing
# and stages them into rootfs/usr/sbin so a fresh flash ships WiFi userspace.
#
# WiFi on the T3 (AP6330/BCM4330), proven working end-to-end 2026-07-16:
#   insmod /system/lib/modules/rkwifi.oob.ko   # OOB-IRQ build (in-band hangs the bus)
#   wpa_supplicant -B -i wlan0 -c wpa.conf -D nl80211
#   udhcpc -i wlan0                            # -> IP + internet
# init.c loads the module at boot; the app (wifi_linux.c) drives wpa_cli/udhcpc.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/thirdparty"
TOOL="$ROOT/toolchain"
CC="zig cc -target arm-linux-musleabihf -mcpu=cortex_a9"
NL="$TP/libnl-tiny"
WPA="$TP/wpa_supplicant-2.10"
WPA_URL="https://w1.fi/releases/wpa_supplicant-2.10.tar.gz"
NL_REPO="https://github.com/openwrt/libnl-tiny"

mkdir -p "$TP"

# 0) Fetch sources on first run (both are gitignored — not ours to commit).
[ -d "$NL" ]  || git clone --depth 1 "$NL_REPO" "$NL"
if [ ! -d "$WPA" ]; then
  echo ">> fetching $WPA_URL"
  curl -fL "$WPA_URL" -o "$TP/wpa.tar.gz"
  tar -xzf "$TP/wpa.tar.gz" -C "$TP"
  rm -f "$TP/wpa.tar.gz"
fi

# 1) libnl-tiny (OpenWrt minimal libnl). NOTE: archive with the ARM toolchain ar,
#    NOT the host macOS ar (which corrupts the ARM objects into a 96-byte stub).
( cd "$NL"
  rm -f ./*.o libnl-tiny.a
  for c in *.c; do $CC -Os -fPIC -D_GNU_SOURCE -Iinclude -c "$c" -o "${c%.c}.o"; done
  "$TOOL/arm-linux-musleabihf-ar" rcs libnl-tiny.a ./*.o )

# 2) wpa_supplicant 2.10 with the internal crypto + libnl-tiny nl80211 backend.
cd "$WPA/wpa_supplicant"
cat > .config <<CFG
CONFIG_DRIVER_NL80211=y
CONFIG_LIBNL_TINY=y
CONFIG_CTRL_IFACE=y
CONFIG_BACKEND=file
CONFIG_TLS=internal
CONFIG_INTERNAL_LIBTOMMATH=y
CONFIG_IEEE80211W=y
CFG
make -j4 \
  CC="$CC" AR="$TOOL/arm-linux-musleabihf-ar" RANLIB="$TOOL/arm-linux-musleabihf-ranlib" \
  EXTRA_CFLAGS="-Os -D_GNU_SOURCE -I$NL/include -Wno-error -Wno-incompatible-function-pointer-types -Wno-int-conversion" \
  LIBS="-static -L$NL -lnl-tiny" LIBS_c="-static -L$NL -lnl-tiny" \
  wpa_supplicant wpa_cli
"$TOOL/arm-linux-musleabihf-strip" wpa_supplicant wpa_cli
echo "built: $(ls -la wpa_supplicant wpa_cli | awk '{print $NF, $5}')"
