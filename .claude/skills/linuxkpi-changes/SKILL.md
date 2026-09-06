---
name: linuxkpi-changes
description: Rules for touching sys/compat/linuxkpi in the NextBSD kernel patch series. Use before adding, editing, or removing any patch under patches/ that changes a LinuxKPI header or source — especially when adding KPI for a driver that lives in nextbsd-kernel-extensions. Covers the struct-layout and include-collision traps that have broken i915 and amdgpu, and the CI blind spot that let both reach main green.
---

# Changing LinuxKPI in the kernel

Every regression this project has shipped to users came from a LinuxKPI patch,
and none of them came from the driver the patch was written for. Read this
before adding one.

## The default answer is: don't put it here

If the KPI exists to serve a driver in `nextbsd-kernel-extensions`, it belongs
**in that module**, not in the kernel. Modules are built with their own include
paths ahead of the kernel's:

	-I${.CURDIR:H}/lkpi                            the module's own
	-I linuxkpi/gplv2/include                      drm-kmod's own
	-I ${SYSDIR}/compat/linuxkpi/common/include    the kernel's

so a header shipped with the module shadows the kernel's for objects compiled
into that module and reaches nothing else. See the `module-private-linuxkpi`
skill in nextbsd-kernel-extensions for how to do it.

Only three things justify a kernel patch, and all three are the same thing:
allocator plumbing that needs state private to `linux_pci.c` (the busdma tags,
the `dma_priv` pctrie, the bounce bookkeeping). That is patches 0012, 0038 and
0043. If your patch is not that, it can almost certainly live in the module.

## Two rules that are not negotiable

### 1. Never add `#include <sys/*.h>` to a LinuxKPI header

LinuxKPI deliberately declares its own types that shadow FreeBSD ones. The one
that bites is `struct resource`: `<linux/ioport.h>` declares a Linux
`struct resource` with `start`/`end`, and `sys/rman.h` declares a completely
different FreeBSD one.

Patch 0050 added `#include <sys/rman.h>` to `<linux/platform_device.h>` so an
inline could call `rman_get_virtual()`. `<linux/mfd/core.h>` includes
`platform_device.h` and then `ioport.h`, so every amdgpu object failed:

	linux/ioport.h:40:8: error: redefinition of 'resource'
	sys/rman.h:102:8: note: previous definition is here

The inline was unsound even where it compiled -- `struct resource` inside it
meant whichever definition was in scope at the call site. If a function needs
FreeBSD types, put the body in a `.c` file that does not include the shadowing
Linux header, and leave only a declaration in the header.

### 2. Never insert a field into an existing shared struct

Not `struct device`, not `struct device_driver`, not `struct pci_dev`.

Patch 0040 inserted `of_node` into `struct device` between `type` and `devt`.
Everything after it -- `devt`, `class`, `release`, `kobj`, `dma_priv`, `irq` --
moved eight bytes. `struct pci_dev` embeds `struct device` first, so every
`pci_dev` member moved too. Any module built before that change reads its
fields eight bytes low. A Dell Wyse 5070 page-faulted in `device_attach()`
loading `i915kms` (gershwin-desktop#49).

Tail-appending is less bad -- existing offsets survive, only `sizeof` grows --
but it is still a KBI break and still requires every module to be rebuilt. The
right answer is not to need the field: keep the mapping module-side. Upstream's
own `dev_of_node()` accessor exists precisely so callers do not touch the field
directly.

## CI does not catch either of the above

`kernel (amd64)` and `kernel (arm64)` build **the kernel**. The kernel does not
reach `<linux/ioport.h>` on the path that collides, and it does not link
prebuilt modules, so both failures above merged to main **green**.

The check that catches them is `drm-kmod build` in
**nextbsd-kernel-extensions**, a different repository. Before merging a patch
that touches `sys/compat/linuxkpi/`, dispatch that workflow and read the
result:

	gh workflow run 286478657 --repo nextbsd/nextbsd-kernel-extensions --ref main

Note the direction of the dependency: that job downloads the kernel obj from
the `continuous` release and compiles against **those** headers. It therefore
cannot test an unmerged kernel PR. Sequence is: merge kernel -> wait for
`publish continuous` -> dispatch the extensions build -> confirm. If it breaks,
you find out after merging, so keep the patch small enough to revert cleanly.

## Working with the patch series

The series is applied in order to a pristine tree. Two failure modes recur:

**Never `git add -A` while the series is applied.** It sweeps all 45 patches
into one. Always: reset hard, apply the series, commit that as a throwaway
BASE, *then* make your edit, then `format-patch -1`. Check the result touches
the files you expect (`grep -cE '^\+\+\+ b/'`) before trusting it.

**Generate a late patch against the tree the earlier ones produce**, not against
a pristine checkout. A patch written against stock `platform_device.h` will not
apply once 0040 has rewritten those lines.

**Removing a patch invalidates the ones after it that touch the same file.**
Removing 0038's header hunk broke 0043, which had context from it. Re-apply the
survivor with `git apply --3way` and regenerate.

**Removing a header hunk can strand a definition.** Dropping `dma-mapping.h`
from 0038 left `linux_pci.c` defining `linux_dma_alloc_wc()` with no prototype;
the kernel builds with `-Wmissing-prototypes`, so arm64 failed. If the body
stays, the declaration stays with it.

## Before you open the PR

- Does this need to be in the kernel at all, or can the module carry it?
- Does it add an `#include <sys/...>` to a header? Move the body to a `.c`.
- Does it change any existing struct? Stop.
- Does the full series still apply? `while read p; do git apply patches/$p; done`
- Does every non-static function it adds have a prototype?
- Has `drm-kmod build` in nextbsd-kernel-extensions been run against it?
