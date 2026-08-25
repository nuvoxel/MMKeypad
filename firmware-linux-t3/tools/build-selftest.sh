#!/usr/bin/env bash
# Cross-build mmk-selftest (audio loopback + camera) for the T3 (armv7 musl,
# static) and stage it into the rootfs at /usr/sbin/mmk-selftest so a flashed
# image ships the SSH-triggerable A/V test. Reuses tinyalsa (thirdparty) + the
# app's validated mixer route (platform/mixer_route.h).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TA="$ROOT/thirdparty/tinyalsa"
ST="$ROOT/tools/selftest"
OUT="$ROOT/rootfs/usr/sbin/mmk-selftest"

ZIG="zig cc"
CFLAGS="-target arm-linux-musleabihf -mcpu=cortex_a9 -static -Os -s -Wall
        -I$TA/include -I$ROOT/platform"

TA_SRCS="$TA/src/pcm.c $TA/src/pcm_hw.c $TA/src/mixer.c $TA/src/mixer_hw.c
         $TA/src/snd_card_plugin.c $TA/src/limits.c"

mkdir -p "$(dirname "$OUT")"
# shellcheck disable=SC2086
$ZIG $CFLAGS -o "$OUT" \
    "$ST/mmk-selftest.c" "$ST/mmk-selftest-camera.c" \
    $TA_SRCS -lm -lpthread
echo "built $OUT ($(ls -la "$OUT" | awk '{print $5}') bytes)"
file "$OUT"
