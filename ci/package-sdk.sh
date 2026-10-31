#!/bin/sh
#
# package-sdk.sh -- pack the kernel SDK: what a kext needs from the kernel to
# be built against it, and nothing else (nextbsd-kernel#327, E21 P2).
#
# The NextBSD analogue of Kernel.framework. A kext build points SYSDIR and
# KERNBUILDDIR at it and compiles its module source (still from freebsd-src)
# without this repo's source tree, its patches, or the ~1 GB kernel obj tree,
# and without relinking the kernel. It is also the boundary the source split
# (E22) relies on: the kext repos depend on this, not on the kernel's tree.
#
#   kernel-sdk/
#     KERNEL-BUILD-ID   the kernel build these headers and options are from
#     KERNEL-VERSION    the kernel version (abi/ABI), e.g. 27.0.0 = kern.osrelease
#     KERNEL-ABI-VERSION  its major.minor, e.g. 27.0: the kext ABI version
#     SDK-INFO          target, kernel commit (kernel_commit=), what is inside
#     sys/              the prepared sys/ tree, minus implementation: headers,
#                       the interface (*.m) and vnode_if sources kmod.mk
#                       generates from, sys/conf (kmod.mk, kern.mk, ...), the
#                       sys/tools scripts it runs, and the kernel config
#     obj/              what the kernel build generated that modules include
#                       (KERNBUILDDIR): opt_*.h, *_if.h, vnode_if*.h,
#                       assym.inc, ...; no objects
#     kernel.symbols    every global symbol the kernel defines: what a kext
#                       can link against
#     kernel.modules    every module version (MODULE_VERSION) compiled into
#                       the kernel: what a kext's MODULE_DEPEND can name
#                       without another kext providing it
#
# usage: package-sdk.sh <target> <target_arch> <out.tar.gz> [srcdir] [objroot]
#   srcdir   the prepared tree the kernel was built from (default /usr/src)
#   objroot  the obj root (default /usr/obj)
# Run after the kernel build and the KERNEL-BUILD-ID stamp, in the same
# container (CROSS_BINDIR for llvm-nm).
#
set -eu

USAGE="usage: package-sdk.sh <target> <target_arch> <out.tar.gz> [srcdir] [objroot]"
TARGET=${1:?$USAGE}
TARGET_ARCH=${2:?$USAGE}
OUT=${3:?$USAGE}
SRC=${4:-/usr/src}
OBJROOT=${5:-/usr/obj}
OBJ="$OBJROOT$SRC/$TARGET.$TARGET_ARCH/sys/NEXTBSD"
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

[ -f "$OBJ/kernel" ] || { echo "package-sdk: no kernel at $OBJ" >&2; exit 1; }
[ -s "$OBJ/KERNEL-BUILD-ID" ] || { echo "package-sdk: no KERNEL-BUILD-ID in $OBJ (stamp it first)" >&2; exit 1; }
[ -f "$SRC/.nextbsd-kernel-prepared" ] || { echo "package-sdk: $SRC is not a prepared tree" >&2; exit 1; }

STAGE=$(mktemp -d)
SDK="$STAGE/kernel-sdk"
mkdir -p "$SDK/sys" "$SDK/obj"

# sys/: interfaces, not implementation. Every header (linuxkpi's and the
# overlay's included), the *.m / *.src files kmod.mk turns into *_if.h and
# vnode_if*.h, and all of sys/conf and sys/tools, which the module build
# includes and runs. Plus the other inputs kmod.mk itself names under
# ${SYSDIR}: firmw.S (firmware modules, FIRMWS), the genassym/genoffset
# generators, the *devs tables (usbdevs, sdiodevs, ...), acpi_quirks and the
# bhnd nvram map. No other .c or .S: module source is not taken from here.
( cd "$SRC/sys" && find . \( -type f -o -type l \) \( \
	-name '*.h' -o -name '*.hh' -o -name '*.m' -o -name '*.src' \
	-o -path './conf/*' -o -path './tools/*' \
	-o -path "./$TARGET/conf/*" \
	-o -path './kern/firmw.S' -o -path './kern/genassym.sh' \
	-o -path './kern/genoffset.c' -o -path './kern/genoffset.sh' \
	-o -path "./$TARGET/$TARGET/genassym.c" \
	-o -path './dev/*/*devs' -o -path './dev/acpica/acpi_quirks' \
	-o -path './dev/bhnd/nvram/nvram_map' -o -path './dev/bhnd/tools/*' \) \
	-print ) | sort > "$STAGE/sys.list"
tar -C "$SRC/sys" -cf - -T "$STAGE/sys.list" | tar -C "$SDK/sys" -xf -

# obj/: the generated, non-object part of the kernel build directory.
( cd "$OBJ" && find . -maxdepth 1 -type f \( \
	-name '*.h' -o -name '*.inc' -o -name 'Makefile' \
	-o -name 'KERNEL-BUILD-ID' -o -name 'version' \) -print ) | sort > "$STAGE/obj.list"
tar -C "$OBJ" -cf - -T "$STAGE/obj.list" | tar -C "$SDK/obj" -xf -
n_opt=$(ls "$SDK/obj"/opt_*.h 2>/dev/null | wc -l | tr -d ' ')
[ "$n_opt" -gt 0 ] || { echo "package-sdk: no opt_*.h in $OBJ" >&2; exit 1; }

NM="${CROSS_BINDIR:-}/llvm-nm"
[ -x "$NM" ] || NM="$(command -v llvm-nm || command -v nm)"
"$NM" -g --defined-only "$OBJ/kernel" | awk '{print $NF}' | sort -u > "$SDK/kernel.symbols"
[ -s "$SDK/kernel.symbols" ] || { echo "package-sdk: no symbols from $OBJ/kernel" >&2; exit 1; }
# MODULE_VERSION(name, ...) leaves a static _mod_metadata_<name>_version in the
# kernel's symbol table; the kernel linker resolves MODULE_DEPEND against
# these versions (plus "kernel" itself).
{ echo kernel; "$NM" --defined-only "$OBJ/kernel" | awk '{print $NF}' |
    sed -n 's/^_mod_metadata_\(.*\)_version$/\1/p'; } | sort -u > "$SDK/kernel.modules"
[ "$(wc -l < "$SDK/kernel.modules")" -gt 1 ] || { echo "package-sdk: no module versions in $OBJ/kernel" >&2; exit 1; }

cp "$OBJ/KERNEL-BUILD-ID" "$SDK/KERNEL-BUILD-ID"

# The kernel version and the kext ABI version (nextbsd-kernel#376).
KVER=$(awk '$1 == "version" { print $2; exit }' "$REPO/abi/ABI")
[ -n "$KVER" ] || { echo "package-sdk: no version in $REPO/abi/ABI" >&2; exit 1; }
echo "$KVER" > "$SDK/KERNEL-VERSION"
echo "${KVER%.*}" > "$SDK/KERNEL-ABI-VERSION"

# The nextbsd-kernel commit this SDK was built from, in full. Kext builds read
# it from SDK-INFO (kernel_commit=); KERNEL-BUILD-ID will stop carrying it
# (#376). safe.directory: the CI container runs as root over a checkout owned
# by the runner user.
KCOMMIT=$(git -c safe.directory='*' -C "$REPO" rev-parse HEAD 2>/dev/null || echo "${GITHUB_SHA:-}")
[ -n "$KCOMMIT" ] || { echo "package-sdk: cannot tell the nextbsd-kernel commit of $REPO" >&2; exit 1; }

{
	echo "target=$TARGET"
	echo "target_arch=$TARGET_ARCH"
	echo "kernconf=NEXTBSD"
	echo "kernel_build_id=$(cat "$OBJ/KERNEL-BUILD-ID")"
	echo "kernel_commit=$KCOMMIT"
	echo "kernel_version=$KVER"
	echo "kernel_abi_version=${KVER%.*}"
	echo "sys_files=$(wc -l < "$STAGE/sys.list" | tr -d ' ')"
	echo "obj_files=$(wc -l < "$STAGE/obj.list" | tr -d ' ')"
	echo "opt_headers=$n_opt"
	echo "kernel_symbols=$(wc -l < "$SDK/kernel.symbols" | tr -d ' ')"
	echo "kernel_modules=$(wc -l < "$SDK/kernel.modules" | tr -d ' ')"
} > "$SDK/SDK-INFO"

tar -C "$STAGE" -czf "$OUT" kernel-sdk
echo "==> kernel SDK $OUT ($(du -h "$OUT" | cut -f1)):"
sed 's/^/    /' "$SDK/SDK-INFO"
