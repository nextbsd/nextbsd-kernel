---
name: freebsd-base-bump
description: Moving NextBSD to a new FreeBSD base (releng/15.1 to 15.2, 16.0, ...), and fixing builds that break on __FreeBSD_version. Use when changing FREEBSD_BRANCH anywhere, when a kernel, world, kext or drm-kmod build fails with a FreeBSD-16-style API or "incompatible function pointer types" after a version change, or when adding code that tests __FreeBSD_version. Covers why NextBSD's __FreeBSD_version is not FreeBSD's, the one pattern for fixing a gate, the audit to run, every place the base is pinned, and the rebuild order.
---

# Moving NextBSD to a new FreeBSD base

## The one thing to internalise

NextBSD's `__FreeBSD_version` is **its own release, not the FreeBSD API it
builds on**. Today it is **2700000** (NextBSD 27.0.0) while the source is
**releng/15.1** (FreeBSD's own value: 1501000). nextbsd-kernel#376/#378 and
nextbsd-freebsd-compat#66 set it; `ci/prepare-src.sh` rewrites it in
`sys/sys/param.h`, and the world sets the same value.

FreeBSD code uses `__FreeBSD_version` as an **API** test: `>= 1600010` means
"FreeBSD 16's API is here". 2700000 passes every such test, so NextBSD takes
code paths for APIs its releng/15.1 tree does not have. Those are the build
breaks this skill is about.

## The rule: two kinds of use, two fixes

| The code asks | Fix | Examples |
|---|---|---|
| **Which API?** (a version gate choosing between FreeBSD code paths) | A plain source patch that **keeps the base's code unconditionally**: drop the FreeBSD clause, or replace the gate with the base's branch. **No build-time defines, no new macros, no `< 2700000` ranges.** | kernel `patches/0106` and world `patches/0002` (nvpair.c), world `0003`-`0005` (devd, ctladm, efiio.h), drm-kmod `0008` (intel_freebsd.c) in nextbsd-kernel-extensions-graphics#109 |
| **Which system?** (identity) | NextBSD's own value (27 / 2700000) | world `0001` (crtbrand: the ELF ABI note is `__FreeBSD_version`), pkg ABI `NextBSD:27:<arch>` (nextbsd-pkg#41, nextbsd-contrib#17), kext ABI `27.0` (`abi/ABI`) |

Name each API patch "keep the releng/X.Y …", say in its message why the gate
flips at NextBSD's value, and keep copies in step: the kernel and the world
compile some files twice (nvpair.c is kernel `0106` = world `0002`).

## Where the base is pinned

| Repo | What to change |
|---|---|
| nextbsd/freebsd-src (fork of freebsd/freebsd-src) | sync the new `releng/X.Y` branch; its daily sync / `Sync Fork` with `force` starts the chain |
| nextbsd-toolchain | `FREEBSD_BRANCH` in `.github/workflows/build-toolchain.yml` (bakes `/usr/src` and the `*-freebsdX.Y` triple into the image) |
| nextbsd-kernel (this repo) | `FREEBSD_BRANCH` in `.github/workflows/build.yml`; `FBSD_VERSION` in `ci/prepare-src.sh`; `abi/ABI`; README's version section |
| nextbsd-freebsd-compat (world) | `FREEBSD_BRANCH` and the `__FreeBSD_version` step in `.github/workflows/build.yml`; `patches/series` |
| nextbsd-kernel-extensions, -graphics, -nvidia | rebuild against the new kernel SDK; graphics: `DRM_KMOD_REF` (the drm-kmod branch matching the base) and `graphics/drm-kmod-patches/`. Note: graphics' `FREEBSD_BRANCH` still reads `releng/15.0`; check what it is used for before trusting it |
| nextbsd-contrib | `build-contrib.sh`: the `case "$SYS_MAJOR"` map from NextBSD major to the triple's FreeBSD release (`27) OSMAJOR=15; OSMINOR=1`). An unmapped major fails on purpose |
| nextbsd-pkg | the `NextBSD:<major>:<arch>` ABI it stamps (only if the NextBSD major changes; see below) |

## Versioning decision first

The kernel README's scheme: `27` is the major while the base is releng/15.1,
and **the next base is 28.0.0**. A new base therefore means deciding, before
any edit:

- NextBSD major: 28 (`__FreeBSD_version` 2800000, `abi/ABI` 28.0.0, kext ABI
  28.0, contrib map `28) OSMAJOR=… OSMINOR=…`).
- The pkg ABI: `NextBSD:27` to `NextBSD:28` changes the repository ABI, so
  installed systems need an upgrade path (pkg refuses a repo whose ABI
  differs). Plan it with nextbsd-pkg before the switch; don't change it as a
  side effect.

Ask the maintainer; don't pick these yourself.

## The audit (run for every base move, and after adding any patch)

Find every gate that evaluates differently at FreeBSD's real value and at
NextBSD's. In a checkout of the new base with NextBSD's patches applied:

```sh
BASE=1502000      # the new base's own __FreeBSD_version, from its sys/sys/param.h
NB=2800000        # NextBSD's value for that base
grep -rnE '__FreeBSD_version[[:space:]]*(>=|>|<=|<|==|!=)[[:space:]]*[0-9]{6,7}' \
    --include='*.[ch]' --include='*.cc' --include='*.cpp' . |
awk -v base="$BASE" -v nb="$NB" '
  { line=$0
    while (match(line, /__FreeBSD_version[ \t]*(>=|>|<=|<|==|!=)[ \t]*[0-9]+/)) {
      e=substr(line, RSTART, RLENGTH); line=substr(line, RSTART+RLENGTH)
      op=e; sub(/^__FreeBSD_version[ \t]*/, "", op); n=op; sub(/^[^0-9]*/, "", n); sub(/[0-9]+$/, "", op); gsub(/[ \t]/, "", op)
      a=(op==">=")?base>=n:(op==">")?base>n:(op=="<=")?base<=n:(op=="<")?base<n:(op=="==")?base==n:base!=n
      b=(op==">=")?nb>=n:(op==">")?nb>n:(op=="<=")?nb<=n:(op=="<")?nb<n:(op=="==")?nb==n:nb!=n
      if (a != b) { print "FLIPS: " $0; break }
    } }'
```

Run it on: the kernel tree (`sys/`, including `sys/contrib/openzfs`), the
world tree (the whole source), and the drm-kmod tag. Ignore Linux-only code
(`__linux__` branches). Every `FLIPS` line needs an API patch (the rule
above) or a written reason it is identity. The awk tests each comparison on
its own, so a compound gate is flagged when any part flips; read it whole.
drm-kmod's `(>= 1500508 && < 1600000) || >= 1600011` is flagged but is true
at both values, so it needs no patch.

Record the results as a table in the PR (site, gate, value at base / at
NextBSD, decision), as nextbsd-kernel-extensions-graphics#109 did for
drm-kmod.

## Step by step

1. **Decide** the NextBSD major and the pkg ABI plan (above).
2. **Toolchain**: bump `FREEBSD_BRANCH` in nextbsd-toolchain; the image's
   triple becomes `*-freebsdX.Y`. Update nextbsd-contrib's major map in the
   same round.
3. **Kernel** (this repo): bump `FREEBSD_BRANCH`, `FBSD_VERSION`, `abi/ABI`.
   Rebase `patches/`: every patch must still `git apply` (prepare-src.sh fails
   on the first that doesn't). Then the audit:
   - an API patch whose gate no longer flips is dropped (example: at a 16.x
     base, nvpair.c's `>= 1600010` branch is the right one, so `0106` goes,
     and so does world `0002`);
   - a new `FLIPS` gets a new API patch.
4. **World** (nextbsd-freebsd-compat): the same rebase and audit over the
   whole tree; keep shared files identical to the kernel's patches.
5. **Kexts**: rebuild all three kext repos on the new kernel SDK; for
   graphics, move `DRM_KMOD_REF` to the drm-kmod branch for the base, rebase
   `graphics/drm-kmod-patches/`, and redo its audit table.
6. **Userland, contrib, pkg, images**: rebuild in that order. The user starts
   nextbsd-pkg and nextbsd/nextbsd builds by hand; ask, don't dispatch.

Merge in dependency order (toolchain, kernel, kexts, world, contrib/userland,
pkg, images), each only after its own CI passes. A kext built for the old
kernel refuses the new one (`MODULE_KERNEL_MAXVER` is derived from
`__FreeBSD_version`), so kernel and kexts ship together.

## What CI proves, and what it doesn't

- **kernel / world / kext builds**: every patch applies and every file
  compiles. A flipped gate usually shows up here as a type error, as nvpair.c
  did (`-Wincompatible-function-pointer-types`).
- **kernel CI's QEMU boot and the ZFS smoke test** (`ci/zfs-smoke.sh`): the
  kernel boots, and a file-backed pool can be created, written, snapshotted,
  exported, imported and scrubbed.
- **Not proved**: a flipped gate that compiles both ways but behaves
  differently at run time. That is why the audit is run on every move, not
  only when a build fails.
