#!/bin/sh
#
# zfs-smoke.sh -- the on-image ZFS check the kernel smoke tests run.
#
# Baked into the PR image as /usr/tests/nextbsd-kernel/zfs-smoke.sh (through
# assemble-image.sh's EXTRA_ROOTFS) and run by the nextbsd-ci harness as the
# one on-image suite (NB_SUITE), as root. It proves the ZFS compiled into THIS
# kernel works with the world's /sbin/zfs and /sbin/zpool: a file-backed pool
# is created, a dataset is written, mounted, snapshotted, exported, imported
# again, scrubbed and exported. No disk is touched; the pool lives in a sparse
# file under /var/tmp, which the throwaway CI image discards.
#
# Protocol (nextbsd-ci manifest/README.md): one MARKER-OK / MARKER-FAIL line
# per check, then `NEXTBSD-TEST-SUMMARY ok=<n> fail=<n> skip=<n>', exit <fail>.
# The workflow's generated overlay lists the ZFS-* markers as must-pass.
#
PATH=/sbin:/bin:/usr/sbin:/usr/bin
export PATH

ok=0
fail=0
skip=0
pass() { printf '%s-OK\n' "$1"; ok=$((ok + 1)); }
flunk() { printf '%s-FAIL\n' "$1"; fail=$((fail + 1)); }
info() { printf 'zfs-smoke: %s\n' "$*"; }
# Run a command, showing it and its output, prefixed so no output line can
# look like a marker.
run() {
	info "+ $*"
	"$@" > /tmp/zfs-smoke.out 2>&1
	rc=$?
	sed 's/^/zfs-smoke:   /' /tmp/zfs-smoke.out
	[ "$rc" -eq 0 ] || info "exit $rc: $*"
	return "$rc"
}

info "kernel: $(uname -v)"

# ZFS-KERNEL: ZFS is in the running kernel. There is no zfs.ko on NextBSD, so
# the sysctl tree and /dev/zfs can only come from the kernel itself.
kver=$(sysctl -n vfs.zfs.version.module 2>/dev/null)
info "vfs.zfs.version.module: ${kver:-<none>}"
if [ -n "$kver" ] && [ -c /dev/zfs ]; then
	pass ZFS-KERNEL
else
	flunk ZFS-KERNEL
fi
run zfs version
if command -v kldstat > /dev/null 2>&1; then
	info "kldstat: modules in the kernel file (id 1) named zfs*:"
	kldstat -v -i 1 2>/dev/null | grep -E '[[:space:]]zfs' | sed 's/^/zfs-smoke:   /'
fi
for ko in /boot/kernel/zfs.ko /boot/modules/zfs.ko; do
	[ -e "$ko" ] && info "note: $ko exists (unexpected on NextBSD)"
done
info "/etc/zfs: $(ls -ld /etc/zfs 2>&1)"
info "ARC before any pool: size=$(sysctl -n kstat.zfs.misc.arcstats.size 2>/dev/null) c_max=$(sysctl -n kstat.zfs.misc.arcstats.c_max 2>/dev/null) physmem=$(sysctl -n hw.physmem)"
vmstat -m 2>/dev/null | awk 'NR == 1 || $1 ~ /^(solaris|zfs)/' | sed 's/^/zfs-smoke:   /'

# ZFS-POOL: a pool on a file vdev, with a dataset.
D=$(mktemp -d /var/tmp/zfs-smoke.XXXXXX) || D=/var/tmp/zfs-smoke
mkdir -p "$D"
if run truncate -s 128m "$D/zt.img" &&
   run zpool create -m none zt "$D/zt.img" &&
   run zfs create zt/a &&
   run zpool list zt; then
	pass ZFS-POOL
else
	flunk ZFS-POOL
fi
run zpool get -H cachefile zt
info "/etc/zfs/zpool.cache: $(ls -l /etc/zfs/zpool.cache 2>&1)"

# ZFS-DATASET-IO: the ZPL. A mounted dataset takes a write and a snapshot,
# and the snapshot is readable through .zfs/snapshot.
if run zfs create -o mountpoint="$D/mnt" zt/b &&
   echo "nextbsd zfs smoke" > "$D/mnt/f" &&
   run sync &&
   run zfs snapshot zt/b@s1 &&
   [ "$(cat "$D/mnt/.zfs/snapshot/s1/f" 2>/dev/null)" = "nextbsd zfs smoke" ] &&
   run zfs list -r -t all zt; then
	pass ZFS-DATASET-IO
else
	flunk ZFS-DATASET-IO
fi
run mount -p

# ZFS-EXPORT-IMPORT: the pool goes away and comes back from its vdev, the
# dataset remounts with its data, and a scrub finds no errors.
if run zpool export zt &&
   run zpool import -d "$D" zt &&
   [ "$(cat "$D/mnt/f" 2>/dev/null)" = "nextbsd zfs smoke" ] &&
   run zpool scrub -w zt &&
   run zpool status -x zt &&
   run zpool export zt; then
	pass ZFS-EXPORT-IMPORT
else
	flunk ZFS-EXPORT-IMPORT
	zpool export -f zt > /dev/null 2>&1
fi

info "ARC after: size=$(sysctl -n kstat.zfs.misc.arcstats.size 2>/dev/null)"
printf 'NEXTBSD-TEST-SUMMARY ok=%d fail=%d skip=%d\n' "$ok" "$fail" "$skip"
exit "$fail"
