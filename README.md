# nextbsd-kernel

NextBSD kernel: source patches (`patches/`), custom config (`config/NEXTBSD`),
and the build workflow. Builds run inside the
[`nextbsd-toolchain`](https://github.com/nextbsd/nextbsd-toolchain)
container, which already carries the exact baked `/usr/src`.

## Layout

```
patches/
  series      # ordered patch list (one filename per line, no comments)
  *.patch     # git format-patch diffs
config/
  NEXTBSD     # kernel config (include GENERIC; ident NEXTBSD)
src-overlay/  # NextBSD's own kernel sources + their sys/conf fragments
abi/
  ABI         # the kernel version, set by hand (see below)
ci/
  prepare-src.sh  # all source preparation (see below)
  package-sdk.sh  # the kernel SDK
.github/workflows/build.yml
```

## Source preparation

`ci/prepare-src.sh <target> <target_arch> <nextbsd-userland checkout> [srcdir]`
turns the toolchain image's unpatched `/usr/src` into the NextBSD kernel tree:
`patches/series`, the NextBSD identity, the regenerated syscall tables, the
config, `src-overlay/` and its `sys/conf` wiring, and the generated MIG servers.
The kernel build runs it; the kext builds no longer do, since they build
against the kernel SDK (below). Anything new that changes the source before a build goes in this
script, not in a workflow step.

## Triggers

- `repository_dispatch: toolchain-updated`: a chain run. It uses the
  SHA-pinned toolchain image and, after publishing, **dispatches
  `kernel-updated` to nextbsd-kernel-extensions and nextbsd-kernel-extensions-graphics**. The chain starts at
  freebsd-src's daily sync when `releng/15.1` moves, or by hand with
  freebsd-src's `Sync Fork` workflow and `force` checked.
- `push` to `main` touching `patches/`, `config/`, `src-overlay/`, `abi/`, `ci/` or the
  workflow: rebuilds and publishes the kernel on the `<arch>-latest` image. It
  does **not** rebuild the kexts; run the chain by hand when a merge should
  reach them.
- `pull_request`: build and boot test only, never publishes or dispatches.
- `workflow_dispatch`: manual; publishes but does not dispatch.

A new push to a PR cancels that PR's previous run. Runs on `main` queue.

## Kernel version and kext ABI

The kernel version is a fixed, Darwin-style release kept in `abi/ABI`
(`version 27.0.0`), not a build stamp (#376). `ci/prepare-src.sh` writes it
into `newvers.sh`, so:

```
kern.osrelease (uname -r)  27.0.0
kern.version   (uname -v)  NextBSD Kernel Version 27.0.0: <date>; root:nextbsd-kernel-<shortsha>~<attempt>/RELEASE_<ARCH>
```

`<shortsha>` is this repo's commit, `<attempt>` the CI run attempt, and
`<ARCH>` is `X86_64`, `ARM64`, or `ARM64_BCM2712` for NEXTBSD-RPI5.
`kern.ostype` stays `NextBSD` and `REVISION` is unchanged.

`ci/prepare-src.sh` also sets `__FreeBSD_version` in `sys/sys/param.h` to
**2700000**, so `kern.osreldate` is 2700000. The world builds with the same
value (nextbsd-freebsd-compat). The kernel and its kexts must agree on it:
each kext's `MODULE_KERNEL_MAXVER` is `roundup(__FreeBSD_version, 100000) - 1`,
so a kext built at 1501000 refuses this kernel. The kext builds run
`prepare-src.sh` too, so they get the same value. The source is still the
releng/15.1 API and the toolchain triple stays `*-freebsd15.1`. Patch `0075`
keeps `gone_in()` comparing against 15, so its "To be removed in FreeBSD N"
notices and `debug.obsolete_panic` behave as on releng/15.1.

The scheme:

- `27` is the major release while NextBSD is based on releng/15.1; the next
  base is 28.0.0.
- `27.0` (major.minor) is the **kext ABI version**. It goes to 27.1.0, 27.2.0…
  when the ABI that kexts build against changes.
- A patch release (27.0.x) is a kernel change that keeps the ABI.

The version is set by hand in `abi/ABI`. It is bumped when the maintainer
decides the ABI kexts build against has changed; no build changes it. The
kernel SDK carries it as `KERNEL-VERSION` (27.0.0) and `KERNEL-ABI-VERSION`
(27.0), and `continuous` publishes both as assets and lists them in the
release notes.

## KERNEL-BUILD-ID

`KERNEL-BUILD-ID` holds the **kext ABI version**, major.minor of `abi/ABI`
(`27.0`), #376. Each build stamps it into `sys/NEXTBSD/KERNEL-BUILD-ID` in its
obj tree, so it is in both `nextbsd-kernel-<arch>.tar.gz` and
`kernel-sdk-<arch>.tar.gz`, and publishes it as the `KERNEL-BUILD-ID` release
asset; it always equals `KERNEL-ABI-VERSION`. Both arches and NEXTBSD-RPI5
share it.

The kext repos copy it from the SDK they built against into every kext's
`NBSDKernelBuildID`, next to `NBSDKernelABIVersion` (from `KERNEL-ABI-VERSION`),
and packaging and the image lanes refuse a kernel and kexts whose IDs differ
(#326). A kernel change that keeps the ABI version therefore needs no kext
rebuild; bumping `abi/ABI` to the next minor makes every kext stale until it is
rebuilt. The key names are kept for the pinned nextbsd-ci and nextbsd-pkg
checks.

Until #376 the ID was `<commit>.<run_id>.<run_attempt>`, one per build. The
build is still identified: the full commit is `kernel_commit=` in the SDK's
`SDK-INFO`, and the short commit and run attempt are in `kern.version`.

## Kernel SDK

The kernel is built with `NO_MODULES=yes`. Kexts are built against
`kernel-sdk-<arch>.tar.gz` (#327, made by `ci/package-sdk.sh`): the kernel's
headers and the module-build inputs `kmod.mk` names, the generated
`opt_*.h`/`*_if.h`, the exported-symbol list, the `KERNEL-BUILD-ID`, the
kernel and ABI versions (above), and `SDK-INFO` with the kernel commit. A
kext repo points `SYSDIR` (and `KERNBUILDDIR` where it wants the kernel's
options) into it, so it needs neither this repo's source nor the obj tree, and
never relinks the kernel. The full obj tree is not published.

See [`patches/README.md`](patches/README.md) for the patch workflow.
