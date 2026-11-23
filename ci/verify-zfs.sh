#!/bin/sh
#
# verify-zfs.sh -- fail when ZFS is not compiled into a built kernel.
#
# config/NEXTBSD sets `options ZFS' (FreeBSD's in-tree OpenZFS, linked into
# the kernel). NextBSD ships no zfs.ko and no /boot/kernel .ko tree, so a
# kernel without it has no ZFS at all, and nothing at runtime would say why.
# Same two layers as the Linuxulator/FUSEFS gate in the workflow:
#   1. objects: config(8) selected the `optional zfs' entries (a lost option
#      or a files.<arch> change drops them with the kernel still linking),
#   2. symbols: they are in the linked kernel, including the arch-specific
#      assembly (sha2/blake3) and the module metadata kldstat/libzfs see.
#
# usage: verify-zfs.sh <target> <kernel obj dir>
#   target   amd64 | arm64 (picks the arch-specific assembly to look for)
#   obj dir  e.g. /usr/obj/usr/src/amd64.amd64/sys/NEXTBSD
# CROSS_BINDIR (the toolchain image's) supplies llvm-nm and llvm-size.
#
set -eu

USAGE="usage: verify-zfs.sh <target> <kernel obj dir>"
TARGET=${1:?$USAGE}
OBJ=${2:?$USAGE}
KERNEL="$OBJ/kernel"
[ -f "$KERNEL" ] || { echo "verify-zfs: no kernel at $KERNEL" >&2; exit 1; }
NM="${CROSS_BINDIR:-}/llvm-nm"
[ -x "$NM" ] || NM="$(command -v llvm-nm || command -v nm || true)"
[ -n "$NM" ] || { echo "verify-zfs: no nm" >&2; exit 1; }
SIZE="${CROSS_BINDIR:-}/llvm-size"
[ -x "$SIZE" ] || SIZE="$(command -v llvm-size || command -v size || true)"

fail=0
echo "--- ZFS objects config(8) should have selected ($(basename "$OBJ"))"
# OS layer, the DMU/SPA core, the ZPL, the SPL, nvpair, lua (channel
# programs), the ICP, zstd, and the shared xdr.
for o in kmod_core.o zfs_vfsops.o zfs_vnops_os.o vdev_geom.o spa.o arc.o \
	 dmu.o zio.o dsl_pool.o txg.o zil.o spl_vfs.o nvpair.o lapi.o \
	 blake3.o sha2_generic.o zfs_zstd.o xdr.o; do
	if [ -f "$OBJ/$o" ]; then echo "  ok    $o"
	else echo "  MISSING $o" >&2; fail=1; fi
done

echo "--- ZFS symbols that must be in the linked kernel"
# Outside the obj dir: that dir is what the kernel tarball packs.
NMOUT=$(mktemp)
"$NM" "$KERNEL" > "$NMOUT" || { echo "verify-zfs: $NM $KERNEL failed" >&2; exit 1; }
# spa_import/spa_create: the pool layer; zfs_vfsops: the VFS; zfsdev_ioctl_common:
# /dev/zfs; arc_init: the ARC; vdev_file_ops/vdev_disk_ops: file and GEOM vdevs;
# _mod_metadata_zfsctrl_version: MODULE_VERSION(zfsctrl), what MODULE_DEPEND
# and kldstat name; zfs_vfsconf: VFS_SET(zfs_vfsops, zfs), the "zfs" module
# libzfs modfind()s before trying to kldload a zfs.ko that does not exist.
for s in spa_import spa_create zfs_vfsops zfsdev_ioctl_common arc_init \
	 vdev_file_ops vdev_disk_ops _mod_metadata_zfsctrl_version zfs_vfsconf; do
	if grep -qE "[[:space:]]$s\$" "$NMOUT"; then echo "  ok    $s"
	else echo "  MISSING symbol $s" >&2; fail=1; fi
done
# One arch-specific assembly routine each, so an arch whose files.<arch>
# entries stopped matching shows up here rather than as a slow generic path.
case "$TARGET" in
amd64) asm="zfs_sha256_transform_x64 zfs_blake3_hash_many_sse2" ;;
arm64) asm="zfs_sha256_block_armv7 zfs_blake3_hash_many_sse2" ;;
*) echo "verify-zfs: unhandled target $TARGET" >&2; exit 1 ;;
esac
for s in $asm; do
	if grep -qE "[[:space:]]$s\$" "$NMOUT"; then echo "  ok    $s (arch asm)"
	else echo "  MISSING symbol $s (arch asm)" >&2; fail=1; fi
done

n=$(grep -cE '[[:space:]][TtDdBbRr][[:space:]]+(zfs_|spa_|arc_|dmu_|dsl_|vdev_|zio_|zil_|zap_|txg_)' "$NMOUT" || true)
echo "--- $n zfs_/spa_/arc_/dmu_/dsl_/vdev_/zio_/zil_/zap_/txg_ symbols in the kernel"
if [ -n "$SIZE" ]; then
	echo "--- size"
	"$SIZE" "$KERNEL" | sed 's/^/  /'
fi
ls -l "$KERNEL" | sed 's/^/  /'

if [ "$fail" -ne 0 ]; then
	echo "" >&2
	echo "ZFS is NOT compiled into $KERNEL." >&2
	echo "config/NEXTBSD must keep 'options ZFS'; check the sys/conf/files*" >&2
	echo "'optional zfs' entries for this arch." >&2
	exit 1
fi
echo "==> ZFS compiled in ($(basename "$OBJ"))"
