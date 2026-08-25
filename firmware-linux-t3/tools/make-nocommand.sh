#!/usr/bin/env bash
#
# make-nocommand.sh -- forge a boot image that drops a T3 into the Android
# recovery "No Command" screen, so we can test the conversion from the state a
# failed customer unit arrives in.
#
# HOW IT WORKS (see reference/t3-control4/JAILBREAK.md): the RK3188 bootloader
# validates the boot partition; a single flipped byte in the kernel makes it
# refuse the image and fall back to Android recovery ("No Command"). Fully
# reversible -- reflash a good boot (our image, or a stock backup) and it's back.
#
# This does NOT touch the device. It writes a corrupted COPY of a boot image;
# you then flash that copy to the boot partition (LBA 40960) the usual way:
#
#   tools/make-nocommand.sh <good-boot.img> [out.img]
#   # then, with the unit in maskrom/loader over USB:
#   rkdeveloptool wl 40960 boot.nocommand.img        # or flash.sh --restore-image
#
# INPUT: a VALID boot image for THIS unit -- ideally this unit's own boot dumped
# first (`rkdeveloptool rl 40960 24576 boot.thisunit`), so the panel/DTB in the
# kernel matches. Any valid T3 boot works to reach recovery, but restoring later
# needs this unit's real kernel, so dump-first is the habit.
set -euo pipefail

SRC="${1:?usage: make-nocommand.sh <good-boot.img> [out.img]}"
OUT="${2:-boot.nocommand.img}"
[ -f "$SRC" ] || { echo "no such file: $SRC" >&2; exit 1; }

# Android boot image layout: 2 KB page holds the "ANDROID!" header, kernel starts
# at page 1. Flip a byte deep in the kernel (0x40000 = 256 KB in) -- well past the
# header so it still PARSES (bootloader reads sizes, attempts the kernel) but the
# kernel itself is corrupt, which is what triggers the recovery fallback. The
# offset is clamped so this is safe on a minimum-size image.
SIZE=$(stat -f%z "$SRC" 2>/dev/null || stat -c%s "$SRC")
OFF=$(( SIZE > 0x80000 ? 0x40000 : SIZE/2 ))

cp "$SRC" "$OUT"
python3 - "$OUT" "$OFF" <<'PY'
import sys
path, off = sys.argv[1], int(sys.argv[2])
with open(path, "r+b") as f:
    f.seek(off)
    b = f.read(1)
    f.seek(off)
    f.write(bytes([b[0] ^ 0xFF]))   # flip every bit of one byte
print(f"flipped byte at 0x{off:x} (0x{b[0]:02x} -> 0x{b[0]^0xff:02x})")
PY

# Sanity: header intact (starts with ANDROID!), size unchanged.
head -c 8 "$OUT" | grep -q "ANDROID" || echo "note: no ANDROID! magic at offset 0 -- is this a raw Android boot image?"
echo "wrote $OUT ($(stat -f%z "$OUT" 2>/dev/null || stat -c%s "$OUT") bytes, header preserved)"
echo
echo "Next, with the unit in maskrom/loader over USB:"
echo "  rkdeveloptool wl 40960 $OUT      # -> reboots to Android recovery 'No Command'"
echo "Reverse it by flashing a good boot: tools/flash.sh  (or --restore / --restore-image)."
