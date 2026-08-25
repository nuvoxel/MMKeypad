#!/usr/bin/env bash
# Cross-build busybox for the T3 (armv7 musl, static) into rootfs/bin/busybox.
#
# WHY THIS EXISTS: the prebuilt busybox that shipped in rootfs/ had a SEGFAULTING
# awk -- `echo "a b" | awk '{print $1}'` died with SIGSEGV (rc 139). busybox's awk
# is the classic victim of strict-aliasing optimisation under clang (which is what
# `zig cc` is): it punts values through unions and casts in ways GCC tolerates and
# clang's aliasing analysis does not. Building the whole tree with
# -fno-strict-aliasing is the standard remedy and costs nothing measurable here.
#
# No device-side code depended on awk (the only two uses in this repo,
# build-wpa.sh and flash.sh, run on the HOST), so this was debug-tooling-only
# breakage -- but it silently corrupts any on-device shell script anyone writes.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/thirdparty"
BB="$TP/busybox-1.36.1"
BB_URL="https://busybox.net/downloads/busybox-1.36.1.tar.bz2"

mkdir -p "$TP"

if [[ ! -d "$BB" ]]; then
  echo "==> fetching busybox 1.36.1"
  curl -fsSL "$BB_URL" -o "$TP/busybox.tar.bz2"
  tar -xjf "$TP/busybox.tar.bz2" -C "$TP"
fi

cd "$BB"
[[ -f .config ]] || { echo "no .config in $BB — configure busybox first" >&2; exit 1; }

# Build awk.c UNOPTIMISED. This fixes a total segfault: at the tree's default -Os,
# `echo "a b" | awk '{print $1}'` dies with SIGSEGV. Verified on hardware
# 2026-07-18: same source, same toolchain, awk.o at -Os segfaults and at -O0
# prints "hello" with rc=0.
#
# ROOT CAUSE, and it is not a compiler bug: building at -O0 makes `zig cc` turn on
# UBSan, and the link then fails on __ubsan_handle_pointer_overflow /
# __ubsan_handle_type_mismatch_v1 referenced from awk.c. So awk.c contains real
# undefined behaviour (pointer overflow, type mismatch), which clang's optimiser is
# entitled to exploit at -Os -- and does, fatally. -fno-strict-aliasing alone does
# NOT help, because aliasing is not the UB in play. -O0 simply stops the optimiser
# acting on it. We disable the sanitizer too, since we want the code to run, not to
# trap, and we are not shipping a UBSan runtime in a 1.3M static busybox.
#
# This papers over the UB rather than fixing it. Upstream busybox builds awk with
# GCC, which happens not to exploit it. If awk ever misbehaves again, the honest
# fix is to find the offending pointer arithmetic in awk.c.
#
# It has to be per-file. kbuild applies CFLAGS_<obj>.o AFTER the global flags
# (scripts/Makefile.lib: _c_flags = $(CFLAGS) $(EXTRA_CFLAGS) $(CFLAGS_$(*F).o)),
# so this overrides -Os for awk.c only. Passing -O0 globally instead ballooned the
# stripped binary from 1.3M to 10M, which matters because busybox ships in the
# ramdisk. awk is not on any hot path, so -O0 costs us nothing real.
for kb in editors/Kbuild.src editors/Kbuild; do
  [[ -f "$kb" ]] || continue
  grep -q '^CFLAGS_awk.o' "$kb" || printf '\nCFLAGS_awk.o := -O0 -fno-sanitize=undefined\n' >> "$kb"
done

# Build through the repo's cross-toolchain wrappers (toolchain/arm-linux-musleabihf-*,
# thin shims over `zig cc` / `zig ar`). CROSS_COMPILE matters for more than the
# compiler: without it busybox's link step reaches for the HOST macOS `ar`, which
# fails with "no archive members specified" on the kbuild built-in.o partial links.
export PATH="$ROOT/toolchain:$PATH"

make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" \
     CROSS_COMPILE=arm-linux-musleabihf- HOSTCC=cc \
     busybox

install -m 0755 "$BB/busybox" "$ROOT/rootfs/bin/busybox"
echo "installed $ROOT/rootfs/bin/busybox ($(du -h "$ROOT/rootfs/bin/busybox" | cut -f1))"
echo "NOTE: reflash the boot image (tools/flash.sh) for this to reach the device —"
echo "      busybox lives in the ramdisk, not the OTA app overlay."
