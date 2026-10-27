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
# smartpqi assigns it to a u_char -> -Werror). So the timestamp goes in RELEASE
# only (= osrelease / uname -r); REVISION stays stock so __FreeBSD__ stays 15.
# Each sed line is self-contained because the version-extraction machinery
# evaluates these lines in isolation. date -u +FMT and date -u are GNU+BSD
# portable.
#
# Also reformat the version banner (VERINFO/VERSTR, both metadata branches) to
# a clean NextBSD string and drop the FreeBSD bits that don't fit out-of-tree:
# the #N build counter and the <hash>[-dirty] VCS tag (the tree is always
# "dirty" -- it's releng/15.1 + the NextBSD patch stack). Result (uname -v):
#   NextBSD Kernel Version <RELEASE>; <date>; <user>@<host>:<objdir>
# RELEASE (the build timestamp) leads, so nextbsd-version -k can still find it.
# Replaced lines are tab-indented inside an if/else; both branches get the same
# value, which is fine.
nv=sys/conf/newvers.sh
sed -i \
	-e 's~^TYPE=.*~TYPE="NextBSD"~' \
	-e 's~^RELEASE=.*~RELEASE=$(date -u +%Y%m%d-%H%M%S)~' \
	-e 's~t=$(date)$~t=$(date -u)~' \
	-e 's~^[[:space:]]*VERINFO=".*~VERINFO="${TYPE} Kernel Version ${RELEASE}; ${t}; ${u}@${h}:${d}"~' \
	-e 's~^[[:space:]]*VERSTR=".*~VERSTR="${VERINFO}\\\\n"~' \
	"$nv"
echo "=== newvers.sh identity after rebrand ==="
grep -nE '^TYPE=|^RELEASE=|t=\$\(date|VERINFO=|VERSTR=' "$nv" || true

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
# publishes evdev input nodes as IOHIDDevice entries.
cat "$REPO/src-overlay/conf/files.ioregistry" >> "$SRC/sys/conf/files"
test -f "$SRC/sys/dev/iokit/iokit_registry.c"
test -f "$SRC/sys/dev/iokit/iokit_hid.c"
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
