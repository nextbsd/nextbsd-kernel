#!/bin/sh
#
# prepare-src.sh -- turn the toolchain image's baked, unpatched /usr/src into
# the NextBSD kernel source tree.
#
# Everything this repo does to the source before a build lives here: the patch
# series, the kernel identity, the regenerated syscall tables, the kernel
# config, src-overlay/ with its sys/conf wiring, and the generated MIG servers.
# The kernel build runs it, and so do the kext builds, at the nextbsd-kernel
# commit whose obj tree they build against (nextbsd-kernel#325). Kernel and
# kexts are then built from the same tree. Before this, the kext builds
# re-applied only patches/series and config/NEXTBSD from nextbsd-kernel HEAD.
# Their buildkernel re-runs config(8) and relinks the kernel, so the option
# headers their modules compiled against lacked everything the overlay adds
# (COMPAT_MACH, COMPAT_LINUX, ...).
#
# usage: prepare-src.sh <target> <target_arch> <userland> [srcdir]
#   target       amd64 | arm64
#   target_arch  amd64 | aarch64
#   userland     a nextbsd-userland checkout (migcom + the MIG wire types)
#   srcdir       the tree to prepare (default /usr/src)
# Runs inside the nextbsd-toolchain container: CROSS_BINDIR must point at the
# cross toolchain (the image sets it), for the sysent step.
#
# Not idempotent (it appends to sys/conf files), so it refuses a tree it has
# already prepared.
#
set -eu

USAGE="usage: prepare-src.sh <target> <target_arch> <userland> [srcdir]"
TARGET=${1:?$USAGE}
TARGET_ARCH=${2:?$USAGE}
ULAND=${3:?$USAGE}
SRC=${4:-/usr/src}
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CONF="$SRC/sys/$TARGET/conf/NEXTBSD"

case "$TARGET" in
amd64|arm64) ;;
*) echo "prepare-src: unhandled target $TARGET" >&2; exit 1 ;;
esac
: "${CROSS_BINDIR:?prepare-src: CROSS_BINDIR is not set (run inside the nextbsd-toolchain image)}"

MARK="$SRC/.nextbsd-kernel-prepared"
if [ -e "$MARK" ]; then
	echo "prepare-src: $SRC was already prepared ($(cat "$MARK")); start from a clean tree" >&2
	exit 1
fi

echo "==> prepare-src: $SRC for $TARGET/$TARGET_ARCH from $REPO ($(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown))"

# ------------------------------------------------------------- patches ----
cd "$SRC"
for p in $(cat "$REPO/patches/series"); do
	echo "Applying $p"
	git apply "$REPO/patches/$p"
done

# ------------------------------------------------------ kernel identity ----
# Done with sed keyed on the stable variable NAMES, NOT a git-apply patch: a
# regex per line is immune to version-string bumps (15.0->15.1, p10->p11) and
# surrounding-code restructuring, whereas git apply needs matching context and
# broke on the p9->p10 bump.
#
# IMPORTANT: REVISION is left STOCK. The build derives the compiler target
# triple (x86_64-unknown-freebsd${OS_REVISION}) from REVISION, and that sets
# __FreeBSD__. A timestamp in REVISION poisons __FreeBSD__ kernel-wide (e.g.
# smartpqi assigns it to a u_char -> -Werror). So the NextBSD version goes in
# RELEASE only (= osrelease / uname -r); REVISION stays stock so __FreeBSD__
# stays 15. Each sed line is self-contained because the version-extraction
# machinery evaluates these lines in isolation; RELEASE is a literal.
#
# The kernel version is a fixed Darwin-style release read from abi/ABI
# (nextbsd-kernel#376), not a build timestamp: kern.osrelease (uname -r) is
# e.g. 27.0.0. It changes when the base or the kext ABI changes, not per build;
# the build itself is named in the banner below.
#
# The version banner (VERINFO/VERSTR, both metadata branches) is reformatted
# to Darwin's shape, dropping the FreeBSD bits that don't fit out-of-tree (the
# #N build counter and the <hash>[-dirty] VCS tag of the throwaway tree):
#   NextBSD Kernel Version 27.0.0: <date>; root:nextbsd-kernel-<shortsha>~<attempt>/RELEASE_<ARCH>
# <shortsha> is this repo's commit, <attempt> the CI run attempt
# (GITHUB_RUN_ATTEMPT, 1 outside CI). <ARCH> is X86_64, ARM64, or
# ARM64_BCM2712 for NEXTBSD-RPI5; newvers.sh knows the config only at build
# time, as ${i} (KERN_IDENT), so nb_arch is set on that same line.
# Replaced lines are tab-indented inside an if/else; both branches get the same
# value, which is fine. REVISION is untouched; __FreeBSD_version is set below.
ABI_VERSION=$(awk '$1 == "version" { print $2; exit }' "$REPO/abi/ABI")
case "$ABI_VERSION" in
[0-9]*.[0-9]*.[0-9]*) ;;
*) echo "prepare-src: no 'version X.Y.Z' line in $REPO/abi/ABI" >&2; exit 1 ;;
esac
# safe.directory: the CI container runs as root over a checkout owned by the
# runner user, which git otherwise refuses ("dubious ownership").
NB_SHA=$(git -c safe.directory='*' -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)
NB_ATTEMPT=${GITHUB_RUN_ATTEMPT:-1}
case "$TARGET" in
amd64) NB_ARCH_EXPR='nb_arch=X86_64' ;;
arm64) NB_ARCH_EXPR='case "${i}" in (NEXTBSD-RPI5) nb_arch=ARM64_BCM2712 ;; (*) nb_arch=ARM64 ;; esac' ;;
esac
nv=sys/conf/newvers.sh
sed -i \
	-e 's~^TYPE=.*~TYPE="NextBSD"~' \
	-e "s~^RELEASE=.*~RELEASE=\"$ABI_VERSION\"~" \
	-e 's~t=$(date)$~t=$(date -u)~' \
	-e "s~^i=\$(\${MAKE:-make} -V KERN_IDENT)\$~&; $NB_ARCH_EXPR~" \
	-e "s~^[[:space:]]*VERINFO=\".*~VERINFO=\"\${TYPE} Kernel Version \${RELEASE}: \${t}; root:nextbsd-kernel-$NB_SHA\\~$NB_ATTEMPT/RELEASE_\${nb_arch}\"~" \
	-e 's~^[[:space:]]*VERSTR=".*~VERSTR="${VERINFO}\\\\n"~' \
	"$nv"
echo "=== newvers.sh identity after rebrand ==="
grep -nE '^TYPE=|^RELEASE=|t=\$\(date|nb_arch=|VERINFO=|VERSTR=' "$nv" || true
grep -q 'KERN_IDENT); .*nb_arch=' "$nv" || { echo "prepare-src: newvers.sh has no KERN_IDENT line to key the banner arch on" >&2; exit 1; }

# __FreeBSD_version = 2700000 (nextbsd-kernel#376): the NextBSD 27.0.0 kernel,
# still on the releng/15.1 API. newvers.sh reads it from param.h into
# osreldate (kern.osreldate), and it is MODULE_VERSION(kernel, ...) and every
# module's MODULE_KERNEL_MAXVER (roundup(__FreeBSD_version, 100000) - 1,
# sys/module.h). Kext builds run this script too, so kernel and kexts flip
# together; a kext built at 1501000 refuses this kernel. The world sets the
# same value (nextbsd-freebsd-compat). Keyed on the define's NAME, not a
# patch, like the identity above, so a releng/15.1 version bump can't break
# it. Only the value changes: newvers.sh and other build files match
# '^#define[[:space:]]*__FreeBSD_version' as a prefix, so no second
# similarly named define may be added.
FBSD_VERSION=2700000
ph=sys/sys/param.h
FBSD_VER_RE='^#define[[:space:]]+__FreeBSD_version[[:space:]]+'
n=$(grep -Ec "${FBSD_VER_RE}[0-9]{7}([[:space:]]|$)" "$ph" || true)
[ "$n" = 1 ] || { echo "prepare-src: $ph: expected one '#define __FreeBSD_version NNNNNNN', found $n" >&2; exit 1; }
sed -i -E "s/(${FBSD_VER_RE})[0-9]{7}([[:space:]]|$)/\1${FBSD_VERSION}\2/" "$ph"
grep -Eq "${FBSD_VER_RE}${FBSD_VERSION}([[:space:]]|$)" "$ph" \
	|| { echo "prepare-src: $ph: __FreeBSD_version is not $FBSD_VERSION after the edit" >&2; exit 1; }
echo "=== param.h __FreeBSD_version ==="
grep -En "${FBSD_VER_RE}" "$ph"

# ------------------------------------------------- syscall tables (sysent) ----
# A syscalls.master patch changes the GENERATED syscall tables (init_sysent.c,
# sys/sys/syscall.h, sysproto.h, syscalls.c, ...). buildkernel compiles the
# committed generated files and does NOT regenerate them, so run the `sysent`
# target in the build env (where flua from the baked kernel-toolchain is on
# PATH). No generated source is ever committed to this repo -- it carries
# patches only. Idempotent when no patch touches syscalls.master. Modules
# include sysproto.h too, so the kext builds need this as much as the kernel.
./tools/build/make.py \
	--cross-bindir="${CROSS_BINDIR}" \
	TARGET="$TARGET" \
	TARGET_ARCH="$TARGET_ARCH" \
	buildenv BUILDENV_SHELL='make -C sys/kern sysent'

# ---------------------------------------------------------- kernel config ----
# config/NEXTBSD in the repo stays clean: the options that only exist because
# of src-overlay/ (COMPAT_MACH, COMPAT_LINUX, LINPROCFS, LINSYSFS) are appended
# to the COPY below, next to the wiring that declares them.
cp "$REPO/config/NEXTBSD" "$CONF"
if [ "$TARGET" = arm64 ]; then
	# The Pi 5 family kernel (`include NEXTBSD`, so it inherits everything
	# appended to the copied NEXTBSD below).
	cp "$REPO/config/NEXTBSD-RPI5" "$SRC/sys/arm64/conf/"
	cp "$REPO/config/rpi5.env" "$SRC/sys/arm64/conf/"
fi
# DTrace's userland provider (pid, USDT). `device fasttrap` only exists on
# amd64 (patch 0070; FreeBSD has no arm64 fasttrap), and config(8) fails on a
# device the arch never declared, so it can't go in the shared config/NEXTBSD.
if [ "$TARGET" = amd64 ]; then
	printf 'device\tfasttrap\n' >> "$CONF"
fi

# --------------------------------------------------------------- overlay ----
# NextBSD-original sources (not freebsd-src), laid into the throwaway tree; the
# fork is never touched.
cp -R "$REPO/src-overlay/sys/." "$SRC/sys/"

# Mach, built INTO the kernel (options COMPAT_MACH, nextbsd#181).
# kern_event.c's knote_enqueue() is static; the kmod resolved it via the
# runtime kld symtab (which includes locals), but the in-kernel static link
# can't. Append a global wrapper in kern_event.c's own TU (it can call the
# static fn); ipc_pset.c calls mach_knote_enqueue.
printf '\n/* nextbsd#181: global wrapper so compat/mach can reach the static knote_enqueue */\nvoid mach_knote_enqueue(struct knote *kn);\nvoid mach_knote_enqueue(struct knote *kn) { knote_enqueue(kn); }\n' >> "$SRC/sys/kern/kern_event.c"
grep -q '^COMPAT_MACH[[:space:]]' "$SRC/sys/conf/options" || \
	printf 'COMPAT_MACH\topt_compat_mach.h\n' >> "$SRC/sys/conf/options"
cat "$REPO/src-overlay/conf/files.compat_mach" >> "$SRC/sys/conf/files"
# COMPAT_MACH_C: extra flags for compat/mach/*.c. -Wno-error mirrors the
# out-of-tree kmod's WERROR= (the in-kernel build defaults to -Werror);
# -DCOMPAT_MACH is omitted (opt_compat_mach.h provides it). $S stays literal
# (single quotes) -- it expands in the kernel Makefile.
printf '\n# mach compat subsystem (options COMPAT_MACH, nextbsd#181)\n' >> "$SRC/sys/conf/kern.pre.mk"
printf 'COMPAT_MACH_C= -DMACH_INTERNAL= -DAUDIT -I$S/apple -include $S/compat/mach/compat_shim.h -Wno-error\n' >> "$SRC/sys/conf/kern.pre.mk"
grep -q '^options[[:space:]]*COMPAT_MACH' "$CONF" || printf 'options\tCOMPAT_MACH\n' >> "$CONF"
echo "==> mach wired in; compat/mach .c files: $(grep -c 'optional compat_mach' "$SRC/sys/conf/files")"

# The Mach MIG servers, generated from their .defs rather than compiled from a
# copy generated in 2015 by a MIG we no longer have (nextbsd-kernel#125, #126).
# Same arrangement as the sysent step above: no generated source is committed
# to this repo. files.compat_mach lists the generated *_server.c, so any build
# of the kernel from this tree needs them. migcom comes from nextbsd-userland
# and is built for the BUILD HOST by gen-mig-servers.sh; the same checkout
# supplies std_types.defs / mach_types.defs, the wire type vocabulary both sides
# of the RPC must share. The kernel workflow then checks the generated servers
# against the frozen msgh_id contract (ci/verify-mig-abi.sh).
#
# The toolchain image runs as root; there is no sudo. Only install the
# generators if they are actually missing.
if ! command -v bison >/dev/null 2>&1 && ! command -v byacc >/dev/null 2>&1; then
	(pkg install -y bison || apt-get update && apt-get install -y bison) || true
fi
if ! command -v flex >/dev/null 2>&1 && ! command -v lex >/dev/null 2>&1; then
	(pkg install -y flex || apt-get install -y flex) || true
fi
"$REPO/ci/gen-mig-servers.sh" "$SRC/sys" "$ULAND"

# In-kernel IOKit catalogue (K2, nextbsd#215). `standard` (always compiled);
# needs no option/config line.
cat "$REPO/src-overlay/conf/files.iocatalogue" >> "$SRC/sys/conf/files"
test -f "$SRC/sys/dev/iokit/iokit_catalogue.c"
test -f "$SRC/sys/sys/iocatalogue.h"
echo "==> iocatalogue wired: $(grep -c 'dev/iokit/iokit_catalogue.c' "$SRC/sys/conf/files") entry; source + header present"

# In-kernel IORegistry (K1, nextbsd#214). `standard`; hangs hw.iokit.registry
# off the shared hw.iokit node defined (now non-static) by iokit_catalogue.c.
# The same fragment lists dev/iokit/iokit_hid.c (`optional evdev`, #335), which
# publishes evdev input nodes as IOHIDDevice entries, and
# dev/iokit/iokit_usb.c (`optional usb`, #391), which publishes every USB
# device as an IOUSBHostDevice entry.
cat "$REPO/src-overlay/conf/files.ioregistry" >> "$SRC/sys/conf/files"
test -f "$SRC/sys/dev/iokit/iokit_registry.c"
test -f "$SRC/sys/dev/iokit/iokit_hid.c"
test -f "$SRC/sys/dev/iokit/iokit_usb.c"
test -f "$SRC/sys/dev/iokit/iokit_drm.c"
test -f "$SRC/sys/sys/ioregistry.h"
echo "==> ioregistry wired: $(grep -c 'dev/iokit/iokit_registry.c' "$SRC/sys/conf/files") entry; source + header present"

# linuxkpi_video compiled into the kernel (graphics autoload, #64). drm.ko
# (IOGraphics.kext) hard-MODULE_DEPENDs on linuxkpi_video -- drm-kmod's video
# helpers (drm_aperture/HDMI/cmdline) -- which FreeBSD ships module-only with
# no config option (no in-tree consumer). NextBSD ships no /boot/kernel .ko
# tree, so its sources compile in here, gated on COMPAT_LINUXKPI. They are
# stock base sources; only the files fragment is appended. iic + lindebugfs,
# drm's other two missing base deps, are in config/NEXTBSD (device iic +
# LINDEBUGFS).
cat "$REPO/src-overlay/conf/files.linuxkpi_video" >> "$SRC/sys/conf/files"
for s in linux_hdmi linux_aperture linux_cmdline linuxkpi_videokmod; do
	test -f "$SRC/sys/compat/linuxkpi/common/src/$s.c"
done
echo "==> linuxkpi_video wired: $(grep -c 'linuxkpi_videokmod.c' "$SRC/sys/conf/files") entry; sources present"

# vfs.pivot primitive (live-root, #70). Self-registers `sysctl vfs.pivot` (no
# base patch) and is inert until written.
cat "$REPO/src-overlay/conf/files.vfs_pivot" >> "$SRC/sys/conf/files"
test -f "$SRC/sys/kern/vfs_pivot.c"
echo "==> vfs.pivot wired: $(grep -c 'kern/vfs_pivot.c' "$SRC/sys/conf/files") entry; source present"

# Darwin kernel events (PF_SYSTEM / SYSPROTO_EVENT, #293). The domain and its
# EVENTHANDLER subscribers self-register (DOMAIN_SET + EVENTHANDLER_DEFINE, no
# base patch) and deliver nothing until a socket sets a filter.
cat "$REPO/src-overlay/conf/files.kev" >> "$SRC/sys/conf/files"
test -f "$SRC/sys/net/kern_event_sock.c"
test -f "$SRC/sys/net/kev_net.c"
echo "==> kernel events wired: $(grep -cE 'net/(kern_event_sock|kev_net)\.c' "$SRC/sys/conf/files") entries; sources present"

# Static Linuxulator: the 64-bit Linux ABI + linprocfs/linsysfs (#60).
# Upstream ships these only as sys/modules/*, and there has never been a static
# path for the 64-bit ABI (COMPAT_LINUX is commented out in
# sys/conf/options.amd64 and absent from options.arm64; neither files.amd64 nor
# files.arm64 has any linux entries). NextBSD ships no /boot/kernel .ko tree and
# no kld tooling, so compiling in is the only route. Sources are stock base
# sources; we append the declarations and the files fragment.
#
# amd64 uses files.compat_linux (amd64/linux/* plus the three x86/linux/*
# files); arm64 uses files.compat_linux.arm64 (arm64/linux/*, no x86 files,
# aarch64 VDSO flags). The split is per-file because config(8) reads one
# files.<arch> per target and has no equivalent of the Makefiles'
# `.if ${MACHINE_CPUARCH} == "amd64"'. On arm64 this also reaches NEXTBSD-RPI5,
# which does `include NEXTBSD'.
#
# COMPAT_LINUX is declared once, MI, so it covers both legs.
cat "$REPO/src-overlay/conf/options.compat_linux" >> "$SRC/sys/conf/options"
case "$TARGET" in
amd64)
	cat "$REPO/src-overlay/conf/files.compat_linux" >> "$SRC/sys/conf/files.amd64"
	# LINPROCFS/LINSYSFS are already declared live in options.amd64;
	# redeclaring them is a "Duplicate option" failure in config(8).
	;;
arm64)
	cat "$REPO/src-overlay/conf/files.compat_linux.arm64" >> "$SRC/sys/conf/files.arm64"
	# options.arm64 declares NEITHER, so unlike amd64 they must be added --
	# per-arch, never MI, or amd64 gets a duplicate.
	cat "$REPO/src-overlay/conf/options.compat_linux.arm64" >> "$SRC/sys/conf/options.arm64"
	;;
esac
for _o in COMPAT_LINUX LINPROCFS LINSYSFS; do
	grep -qE "^options[[:space:]]+${_o}\b" "$CONF" || printf 'options\t%s\n' "$_o" >> "$CONF"
done
# We declare COMPAT_LINUX ourselves, so it must still be commented out (amd64)
# or absent (arm64) upstream, and the per-arch LINPROCFS/LINSYSFS state must be
# what each branch above assumes. Either assumption breaking on a FreeBSD bump
# is a "Duplicate option" or "unknown option" failure in config(8), so assert
# it here where the message is legible.
grep -qE '^#COMPAT_LINUX[[:space:]]' "$SRC/sys/conf/options.amd64"
if [ "$TARGET" = amd64 ]; then
	grep -qE '^LINPROCFS[[:space:]]' "$SRC/sys/conf/options.amd64"
	grep -qE '^LINSYSFS[[:space:]]' "$SRC/sys/conf/options.amd64"
	_mdsrc="amd64/linux"
else
	# Must be absent from the pristine file -- we append them ourselves, so an
	# upstream addition would become a duplicate.
	if git -C "$SRC" show HEAD:sys/conf/options.arm64 \
	    | grep -qE '^(LINPROCFS|LINSYSFS)[[:space:]]'; then
		echo "options.arm64 now declares LINPROCFS/LINSYSFS upstream;" >&2
		echo "drop options.compat_linux.arm64 or config(8) will duplicate." >&2
		exit 1
	fi
	_mdsrc="arm64/linux"
fi
for s in $_mdsrc/linux_sysvec.c $_mdsrc/linux_genassym.c \
	 $_mdsrc/linux_locore.asm $_mdsrc/linux_vdso_gtod.c \
	 $_mdsrc/linux_vdso.lds.s compat/linux/linux_vdso_inc.S \
	 compat/linux/linux_elf64.c compat/linprocfs/linprocfs.c \
	 compat/linsysfs/linsysfs.c; do
	test -f "$SRC/sys/$s" || { echo "MISSING $SRC/sys/$s" >&2; exit 1; }
done
n=$(grep -c 'optional compat_linux' "$SRC/sys/conf/files.$TARGET" || true)
echo "==> Linuxulator wired ($TARGET): $n entries in files.$TARGET; sources present"

echo "nextbsd-kernel $(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown) $TARGET" > "$MARK"
echo "==> prepare-src: done"
