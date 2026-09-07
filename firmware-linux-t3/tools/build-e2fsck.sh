#!/usr/bin/env bash
# Cross-build a STATIC e2fsck for the T3 (armv7 musl) into rootfs/sbin/e2fsck.
#
# WHY THIS EXISTS: the T3 has no way to repair its own filesystem. busybox ships
# only `fsck` (a wrapper that needs fsck.ext4) and `fsck.minix`; the stock
# /system/bin/tune2fs is a 2008 Android binary that segfaults under our rootfs.
# So a `userdata` partition whose ext4 journal gets damaged -- which is what a
# `reboot -f` does, since a sync is not an unmount -- cannot be mounted OR fixed
# on the device. The panel then boots with /data unmounted, silently falls back to
# the factory app, and every setting it writes lives on a ramdisk until reboot.
#
# Recovery is one command (`e2fsck -fy /dev/mtdblock6`) IF the binary is there.
# Ship it so it always is.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/thirdparty"
VER="1.47.0"
SRC="$TP/e2fsprogs-$VER"
URL="https://mirrors.edge.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v$VER/e2fsprogs-$VER.tar.gz"
TARGET="arm-linux-musleabihf"

mkdir -p "$TP"
if [ ! -d "$SRC" ]; then
  echo "==> fetching e2fsprogs $VER"
  curl -fsSL "$URL" -o "$TP/e2fsprogs.tar.gz"
  tar -xzf "$TP/e2fsprogs.tar.gz" -C "$TP"
fi

# PATCH: ext2fs_check_if_mounted() treats EBUSY from open(dev, O_RDONLY|O_EXCL) as
# proof the device is mounted. The T3's RK3188 kernel returns EBUSY for an mtdblock
# device whether or not it is mounted, so e2fsck refuses every repair with
# "/dev/mtdblock6 is in use. Cannot continue" -- on a partition that /proc/mounts,
# /etc/mtab and `mount` all agree is NOT mounted. There is no override flag, so drop
# the false positive here and let /proc/mounts (which is accurate) decide.
# ALWAYS confirm the target really is unmounted before running this binary.
ISM="$SRC/lib/ext2fs/ismounted.c"
if grep -q 'busy = 1;' "$ISM"; then
  echo "==> patching ismounted.c (mtdblock EBUSY false positive)"
  perl -0pi -e 's/\} else if \(errno == EBUSY\) \{\n\t\t\t\tbusy = 1;\n/} else if (errno == EBUSY) {\n\t\t\t\t\/* T3: mtdblock always EBUSY; see build-e2fsck.sh *\/\n/' "$ISM"
  grep -q 'T3: mtdblock always EBUSY' "$ISM" || { echo "patch FAILED"; exit 1; }
fi

# Same kernel quirk, second place: unix_io.c adds O_EXCL to its own open() when
# EXT2_FLAG_EXCLUSIVE is set (e2fsck always sets it), which fails with "Resource busy
# while trying to open /dev/mtdblock6" for the same bogus reason. Drop it here too.
UIO="$SRC/lib/ext2fs/unix_io.c"
if grep -q 'open_flags |= O_EXCL;' "$UIO"; then
  echo "==> patching unix_io.c (drop O_EXCL on open)"
  perl -pi -e 's/^\t\topen_flags \|= O_EXCL;/\t\t\/* T3: mtdblock always EBUSY on O_EXCL; see build-e2fsck.sh *\//' "$UIO"
  grep -q 'T3: mtdblock always EBUSY on O_EXCL' "$UIO" || { echo "unix_io patch FAILED"; exit 1; }
fi

BUILD="$SRC/build-$TARGET"
mkdir -p "$BUILD"
cd "$BUILD"

# zig cc IS clang. e2fsprogs' configure probes for a working cross compiler, so give
# it the full target on every tool. -fno-strict-aliasing for the same reason
# build-busybox.sh needs it: this is old C that clang's aliasing analysis punishes.
export CC="zig cc -target $TARGET -mcpu=cortex_a9 -fno-strict-aliasing"
export AR="zig ar"
export RANLIB="zig ranlib"
# musl HAS lseek64, but configure's cross link-test cannot prove it and leaves
# HAVE_LSEEK64 undefined -- which drops lib/blkid/llseek.c into a 1990s _syscall5
# fallback that clang rejects outright ("type specifier missing"). Assert it.
export CFLAGS="-Os -static -D_LARGEFILE64_SOURCE -DHAVE_LSEEK64=1"
export LDFLAGS="-static"

if [ ! -f Makefile ]; then
  echo "==> configure"
  ../configure \
    --host="$TARGET" \
    --disable-nls --disable-uuidd --disable-tls --disable-fuse2fs \
    --enable-libuuid --enable-libblkid >/dev/null
    # --enable-libuuid/--enable-libblkid build e2fsprogs' OWN bundled copies.
    # The --disable- forms mean "use an external one", which there is no cross-built
    # copy of here, and configure dies with "external uuid library not found".
fi

echo "==> building e2fsck"
# `libs` is e2fsprogs' own aggregate target — lib/ has no Makefile of its own, only
# per-library subdirs whose ordering the top level knows.
J="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
make -j"$J" libs
make -j"$J" -C e2fsck e2fsck

OUT="$ROOT/rootfs/sbin/e2fsck"
mkdir -p "$(dirname "$OUT")"
cp e2fsck/e2fsck "$OUT"
chmod 755 "$OUT"
file "$OUT" || true
echo "built $OUT"
