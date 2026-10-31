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
#     SDK-MANIFEST      "sha256  path" for every SDK file that defines the
#                       kext ABI, plus the target triple and compiler version
#     SDK-HASH          sha256 of SDK-MANIFEST, checked against abi/ABI
#     SDK-MANIFEST-NEXTBSD-RPI5, SDK-HASH-NEXTBSD-RPI5
#                       arm64 only: the same for the NEXTBSD-RPI5 kernel's
#                       generated headers and modules
#     SDK-INFO          target, kernel commit, what is inside
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
OBJ_RPI5="$OBJROOT$SRC/$TARGET.$TARGET_ARCH/sys/NEXTBSD-RPI5"
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

# SDK-MANIFEST: what the kext ABI is made of, one "sha256  path" line per file,
# sorted by path (LC_ALL=C), then the compiler target triple and version. Its
# sha256 is SDK-HASH. Two builds of the same commit must give the same hash, so
# nothing build-specific goes in: obj/version (the build counter), newvers.sh
# (it carries the commit and run attempt) and the KERNEL-*/SDK-* files are
# left out. The triple is the one the build derives from REVISION.
case "$TARGET_ARCH" in
amd64) TRIPLE_ARCH=x86_64 ;;
*) TRIPLE_ARCH=$TARGET_ARCH ;;
esac
REVISION=$(sed -n 's/^REVISION="\(.*\)"$/\1/p' "$SRC/sys/conf/newvers.sh")
[ -n "$REVISION" ] || { echo "package-sdk: no REVISION in $SRC/sys/conf/newvers.sh" >&2; exit 1; }
TRIPLE="$TRIPLE_ARCH-unknown-freebsd$REVISION"
CC_BIN="${CROSS_BINDIR:-}/cc"
[ -x "$CC_BIN" ] || CC_BIN="${CROSS_BINDIR:-}/clang"
CC_VERSION=$("$CC_BIN" -dumpversion)
[ -n "$CC_VERSION" ] || { echo "package-sdk: no version from $CC_BIN" >&2; exit 1; }

# usage: sdk_manifest <kernel obj dir> <kernel.modules file>
# The sys/ part comes from the staged SDK; obj/ from the given obj dir, so the
# NEXTBSD-RPI5 manifest can name its own generated headers under obj/.
sdk_manifest() {
	{
		( cd "$SDK" && {
			find sys -type f \( -name '*.h' -o -name '*.hh' -o -name '*.m' \
			    -o -name '*.src' \) -print
			for f in kmod.mk kern.mk kern.opts.mk kern.pre.mk; do
				echo "sys/conf/$f"
			done
			find sys/conf -maxdepth 1 -type f -name 'ldscript.*' -print
		  } | grep -vxE 'sys/conf/newvers\.sh' | tr '\n' '\0' | xargs -0r sha256sum )
		# Symlinks by their target, so a dangling one cannot fail the hash.
		( cd "$SDK" && find sys -type l \( -name '*.h' -o -name '*.hh' \
		    -o -name '*.m' -o -name '*.src' \) -print ) | while read -r l; do
			printf '%s  %s\n' "$(readlink "$SDK/$l" | sha256sum | cut -d' ' -f1)" "$l"
		done
		( cd "$1" && find . -maxdepth 1 -type f \( -name '*.h' -o -name '*.inc' \) \
		    -print0 | xargs -0r sha256sum ) | sed 's~  \./~  obj/~'
		printf '%s  kernel.modules\n' "$(sha256sum < "$2" | cut -d' ' -f1)"
	} | grep -vE '  (obj/version|sys/conf/newvers\.sh|KERNEL-[^/]*|SDK-[^/]*)$' \
	  | LC_ALL=C sort -k2,2
	echo "target=$TRIPLE"
	echo "cc=$CC_VERSION"
}
sdk_manifest "$OBJ" "$SDK/kernel.modules" > "$SDK/SDK-MANIFEST"
sha256sum < "$SDK/SDK-MANIFEST" | cut -d' ' -f1 > "$SDK/SDK-HASH"

# NEXTBSD-RPI5 (arm64, built before this runs) has its own opt_*.h and its own
# set of compiled-in modules, so its kexts see a different ABI unless the two
# hashes agree. Only the manifest and hash are shipped, not its headers.
if [ -f "$OBJ_RPI5/kernel" ]; then
	"$NM" --defined-only "$OBJ_RPI5/kernel" | awk '{print $NF}' |
	    sed -n 's/^_mod_metadata_\(.*\)_version$/\1/p' > "$STAGE/rpi5.mods"
	{ echo kernel; cat "$STAGE/rpi5.mods"; } | sort -u > "$STAGE/rpi5.modules"
	sdk_manifest "$OBJ_RPI5" "$STAGE/rpi5.modules" > "$SDK/SDK-MANIFEST-NEXTBSD-RPI5"
	sha256sum < "$SDK/SDK-MANIFEST-NEXTBSD-RPI5" | cut -d' ' -f1 > "$SDK/SDK-HASH-NEXTBSD-RPI5"
fi
{
	echo "target=$TARGET"
	echo "target_arch=$TARGET_ARCH"
	echo "kernconf=NEXTBSD"
	echo "kernel_build_id=$(cat "$OBJ/KERNEL-BUILD-ID")"
	echo "kernel_version=$KVER"
	echo "kernel_abi_version=${KVER%.*}"
	echo "sdk_hash=$(cat "$SDK/SDK-HASH")"
	[ ! -s "$SDK/SDK-HASH-NEXTBSD-RPI5" ] ||
		echo "sdk_hash_nextbsd_rpi5=$(cat "$SDK/SDK-HASH-NEXTBSD-RPI5")"
	echo "sdk_manifest_entries=$(wc -l < "$SDK/SDK-MANIFEST" | tr -d ' ')"
	echo "sys_files=$(wc -l < "$STAGE/sys.list" | tr -d ' ')"
	echo "obj_files=$(wc -l < "$STAGE/obj.list" | tr -d ' ')"
	echo "opt_headers=$n_opt"
	echo "kernel_symbols=$(wc -l < "$SDK/kernel.symbols" | tr -d ' ')"
	echo "kernel_modules=$(wc -l < "$SDK/kernel.modules" | tr -d ' ')"
} > "$SDK/SDK-INFO"

tar -C "$STAGE" -czf "$OUT" kernel-sdk
echo "==> kernel SDK $OUT ($(du -h "$OUT" | cut -f1)):"
sed 's/^/    /' "$SDK/SDK-INFO"
