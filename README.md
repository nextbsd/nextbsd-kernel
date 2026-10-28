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
ci/
  prepare-src.sh  # all source preparation (see below)
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
- `push` to `main` touching `patches/`, `config/`, `src-overlay/`, `ci/` or the
  workflow: rebuilds and publishes the kernel on the `<arch>-latest` image. It
  does **not** rebuild the kexts; run the chain by hand when a merge should
  reach them.
- `pull_request`: build and boot test only, never publishes or dispatches.
- `workflow_dispatch`: manual; publishes but does not dispatch.

A new push to a PR cancels that PR's previous run. Runs on `main` queue.

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
`opt_*.h`/`*_if.h`, the exported-symbol list and the `KERNEL-BUILD-ID`. A
kext repo points `SYSDIR` (and `KERNBUILDDIR` where it wants the kernel's
options) into it, so it needs neither this repo's source nor the obj tree, and
never relinks the kernel. The full obj tree is not published.

See [`patches/README.md`](patches/README.md) for the patch workflow.
