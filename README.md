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
  ABI         # kernel version + the kext ABI's SDK hashes (see below)
ci/
  prepare-src.sh  # all source preparation (see below)
  package-sdk.sh  # the kernel SDK, its manifest and hash
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
`kern.ostype` stays `NextBSD`; `REVISION`, `__FreeBSD_version` and
`kern.osreldate` are unchanged.

The scheme:

- `27` is the major release while NextBSD is based on releng/15.1; the next
  base is 28.0.0.
- `27.0` (major.minor) is the **kext ABI version**. It goes to 27.1.0, 27.2.0…
  when the ABI that kexts build against changes.
- A patch release (27.0.x) is a kernel change that keeps the ABI.

The ABI is measured, not declared. `ci/package-sdk.sh` writes `SDK-MANIFEST`
into the kernel SDK. It is a sorted `sha256  path` list over the SDK's headers
and `*.m`/`*.src` interface files, the `kmod.mk`/`kern*.mk` files and
`ldscript.*`, the generated `obj/*.h` and `obj/*.inc`, and `kernel.modules`,
followed by the compiler target triple and `cc -dumpversion`. Its sha256 is
`SDK-HASH`. Nothing build-specific is in it (`obj/version`, `newvers.sh` and
the `KERNEL-*`/`SDK-*` files are left out), so two builds of one commit give
the same hash. On arm64 the NEXTBSD-RPI5 kernel gets its own
`SDK-MANIFEST-NEXTBSD-RPI5`/`SDK-HASH-NEXTBSD-RPI5`. The SDK also carries
`KERNEL-VERSION` (27.0.0) and `KERNEL-ABI-VERSION` (27.0). `continuous`
publishes those two, plus `SDK-HASH-amd64`, `SDK-HASH-arm64` and
`SDK-HASH-arm64-rpi5`, as assets and in the release notes.

`abi/ABI` records the expected hashes (`sdk-amd64`, `sdk-arm64`,
`sdk-arm64-rpi5`). The build's guard step compares each computed hash with
it. On a mismatch it prints the manifest diff against the published SDK,
which names the files that changed. The fix is to bump `version` to
27.<n+1>.0 and record the new hashes the step prints. It also flags a PR that
changes the recorded hashes without changing major.minor. **The guard is
warn-only for now** (a `::warning::`, never a failure). It will enforce once
rebuilds are shown to give the same hash and the kexts key on the ABI
version.

## KERNEL-BUILD-ID

Each build stamps `<commit>.<run_id>.<run_attempt>` into
`sys/NEXTBSD/KERNEL-BUILD-ID` in its obj tree, so it is in both
`nextbsd-kernel-<arch>.tar.gz` and `kernel-sdk-<arch>.tar.gz`, and publishes it
as the `KERNEL-BUILD-ID` release asset. Both arches and NEXTBSD-RPI5 share the
run's ID. It is stronger than `__FreeBSD_version`: the same commit rebuilt is a
new ID. The kext repos copy the ID from the SDK they built against into every
kext, and packaging and the image lanes refuse a kernel and
kexts whose IDs differ (#326).

## Kernel SDK

The kernel is built with `NO_MODULES=yes`. Kexts are built against
`kernel-sdk-<arch>.tar.gz` (#327, made by `ci/package-sdk.sh`): the kernel's
headers and the module-build inputs `kmod.mk` names, the generated
`opt_*.h`/`*_if.h`, the exported-symbol list, the `KERNEL-BUILD-ID`, the
kernel and ABI versions and the ABI manifest and hash (above). A
kext repo points `SYSDIR` (and `KERNBUILDDIR` where it wants the kernel's
options) into it, so it needs neither this repo's source nor the obj tree, and
never relinks the kernel. The full obj tree is not published.

See [`patches/README.md`](patches/README.md) for the patch workflow.
