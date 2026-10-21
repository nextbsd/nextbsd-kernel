---
name: mach-kernel-changes
description: Rules for changing the Mach layer of the NextBSD kernel. Use before adding or changing anything under src-overlay/sys/compat/mach, src-overlay/sys/sys/mach/*.defs or *.msgids, ci/gen-mig-servers.sh or ci/verify-mig-abi.sh, or before adding or wiring a Mach syscall trap. Covers where a change belongs (patches/ vs src-overlay/ vs build.yml), the MIG-first decision gate in front of any new trap, the .defs → generated server workflow and its silent msgh_id renumbering trap, and what the CI checkmark does and does not prove.
---

# Changing the Mach layer in the kernel

The default way to reach the kernel from libmach is **MIG**, not a new trap.
Every kernel RPC rides the one already-wired `mach_msg_trap`, costs zero
syscall slots, and has a `.defs` here that both sides are generated from. A
trap is an optional accelerator over a MIG path that already works, the way
Apple does it. Read the decision gate below before you write one.

## Where a change belongs

**`patches/` patches FreeBSD's own kernel. `src-overlay/` is NextBSD's own
kernel code.** The rule is about intent, not about whether a file is new:
FreeBSD-shaped drivers (`0015`, `0018`, `0025`) add new files as patches.

| Change | Goes in |
|---|---|
| Edit an existing freebsd-src file | `patches/` + a line in `patches/series` (`patches/README.md`) |
| Anything under `compat/mach`, `sys/mach`, `sys/apple`, `dev/iokit` | `src-overlay/sys/…` + a line in the matching `src-overlay/conf/files.*` |
| Option stock FreeBSD declares | `config/NEXTBSD` |
| Option only the overlay declares (`COMPAT_MACH`, `COMPAT_LINUX`, …) | a wiring step in `build.yml`, **never** `config/NEXTBSD` |

Mechanically: the series is `git apply`'d in `series` order (no comments, no
blank lines, not numerically sorted). Then the "Build mach into the kernel"
step `cp -R`s `src-overlay/sys/.` over `/usr/src/sys/` (`build.yml:111`) and
wires `COMPAT_MACH` into `sys/conf/options`, `sys/conf/files`,
`kern.pre.mk` and the copied config (`build.yml:117-128`). A patch can never
depend on an overlay file. The overlay may assume patched sources, and does.

**The last row is the one that bites.** nextbsd-kernel-extensions reuses
`patches/series` and `config/NEXTBSD` to build a KBI-matched kernel and lays
in none of `src-overlay/`. An overlay-only option named in `config/NEXTBSD`
makes its `config(8)` die with `unknown option`. That broke both legs of the
modules build from 2026-08-03 while this repo stayed green
(`config/NEXTBSD:132-140`, `build.yml:100-107`).

**Some kernel code is injected by `build.yml` itself.** The global
`mach_knote_enqueue()` wrapper is `printf`'d onto the end of `kern_event.c`
(`build.yml:112-116`). Grep `build.yml` before deciding a symbol is missing.

Patches target freebsd-src **`releng/15.1`** (`FREEBSD_BRANCH`,
`build.yml:15`). `patches/README.md` still says an empty series builds
"stock `releng/15.0`"; it is stale.

## The decision gate: before you add a trap

Answer both questions first. A "yes" to either means you do not write a trap.

**(a) Does XNU expose this routine via MIG?** Check Apple's `.defs` and XNU's
`mach_trap_table` (130 entries). If Apple reaches it by MIG, so do we.

**(b) Does a `.defs` and a generated server already exist here?** Today all
seven subsystems are generated: `gen_one` × 7 at the end of
`ci/gen-mig-servers.sh` (`:243-290`), dispatched through `mig_e[]` in
`compat/mach/ipc/ipc_kobject.c:204-229`.

If a MIG path exists, implement the kernel handler and call it through the
MIG client. A trap comes later, only as an accelerator over that working
client, falling back to MIG on `MACH_SEND_INVALID_DEST` as Apple's does.

The arithmetic is why this is not a style preference. Patch 0001 widened
FreeBSD's `lkmnosys` band from 10 slots to 58 because "lkmnosys slots
(210-219) are exhausted by the Mach trap set"
(`patches/0001-NextBSD-widen-lkmnosys-dynamic-syscall-band-to-58-sl.patch:26`).
The seven subsystems carry roughly 156 routines. One slot each never fits.
`mach_syscall_wire_register()` already wires 11 traps
(`compat/mach/mach_syscall_wire.c:545-582`). The three that had no Apple
counterpart (`task_get_special_port`, `task_set_special_port`,
`host_set_special_port`) were converted to MIG and their traps deleted
(nextbsd-userland#84); their slots stay `lkmnosys`.

### A generated server is not a working routine

Most kernel handlers behind the servers are `UNSUPPORTED`
(`sys/mach/std_types.h:169-173`). That returns `KERN_NOT_SUPPORTED`, or
**panics** under `MACH_DEBUG && INVARIANTS`. See `mach_host.c:41-110`,
`mach_host_priv.c:55-140`, `mach_task.c:77-95`, `mach_semaphore.c:46-60`.
`mach_host.defs` records that 16 of its 17 routines are `UNSUPPORTED`. Before
wiring a client to a routine, read its handler.

## The `.defs` workflow

### How generation works

`ci/gen-mig-servers.sh <srcdir> <userland-checkout>` runs in the "Generate MIG
servers from .defs" step. It:

1. Builds a **host** `migcom` from
   `nextbsd-userland/src/bootstrap_cmds/migcom.tproj`. The kernel workflow
   checks out userland **`main`** for this ("Checkout nextbsd-userland",
   `build.yml:151-156`). The target-arch migcom in the userland tarball is
   useless when the x86 runner cross-builds arm64.
2. Takes the wire-type vocabulary (`std_types.defs`, `mach_types.defs`) from
   userland's `src/libmach/include`. Every subsystem `.defs` comes from
   `src-overlay/sys/sys/mach/`.
3. For each `gen_one <sub>`, writes `/usr/src/sys/compat/mach/<sub>_server.c`
   and `sys/mach/<sub>_gen.h`, then rewrites the include prologue to the
   kernel set (`:133-240`).

The 2015 servers were **hand-adapted**, and pure MIG output does not reproduce
them. The script reapplies that adaptation mechanically: `<mach/X.h>` →
`<sys/mach/X.h>`, drop `<string.h>` and `<mach/boolean.h>`, splice in six
NextBSD kernel headers stock MIG never emits (`ipc_sync.h`, `ipc_voucher.h`,
`ipc_host.h`, `ipc_tt.h`, `ipc_mig.h`, `mach_debug_types.h`), plus one extra
for `task` and `vm_map`. If migcom's output changes shape, those steps fail
loudly; fix the transform, do not loosen the check.

**`KERNEL_SERVER` must be defined** (`:117-125`). It turns on the intran /
destructor translations in `mach_types.defs`. Without it the dispatch passes
the raw request port where an `ipc_space_t` is expected and the kernel
page-faults in `ipc_right_lookup()`.

### The `.msgids` contract

`msgh_id` is **subsystem base + routine index**. A misplaced or missing
`skip;` silently renumbers every routine after it, and the kernel then runs
the **wrong function** for a message. No error and no crash.

So the generated C is not committed. A `.msgids` file is
(`src-overlay/sys/sys/mach/<sub>.msgids`, format `<msgid> <routine|skip>
<argc>` under a `subsystem <name> <base> <end>` line). `ci/verify-mig-abi.sh
<server.c> <contract.msgids>` checks base, end and every slot, tombstones
included, on every build ("Verify MIG ABI against the frozen contract",
`build.yml:176-193`). Changing a `.msgids` is an ABI break. If it is
intentional, change it in the same commit and say why.

### Passing the ABI gate does not mean it compiles

They check different things. The gate reads the routine table; it never
compiles anything. A generated server has passed the gate and then failed to
build on `ipc_info_space_basic_t`, `IP_VALID` and
`ipc_port_check_circularity`, which is where the six spliced headers came
from. Wait for the `kernel` job on both arches.

### Converting or adding a subsystem

1. Write `src-overlay/sys/sys/mach/<sub>.defs`. Routine order is load-bearing.
   Wrap `KernelServer` in `#if NEXTBSD_KERNEL_SERVER` as the existing ones do.
2. Add `src-overlay/sys/sys/mach/<sub>.msgids`. For a conversion, capture it
   from the frozen server **before** you replace it.
3. Add `gen_one <sub>` to `ci/gen-mig-servers.sh`, and a header case there if
   the server's own entry points need one.
4. Add `<sub>` to the verify loop in `build.yml` (`for sub in …`, `:189`).
5. New subsystem only: list `compat/mach/<sub>_server.c` in
   `src-overlay/conf/files.compat_mach` and add `&<sub>_subsystem` to
   `mig_e[]` in `ipc_kobject.c`.

### UserPrefix and the libmach client

`mach_port.defs`, `task.defs`, `host_priv.defs` and `mach_host.defs` set
`UserPrefix _kernelrpc_` (`mach_port.defs:88`, `task.defs:109`,
`host_priv.defs:127`, `mach_host.defs:105`). Their generated clients sit under
libmach's public wrappers instead of colliding with them: libmach defines
`task_get/set_special_port`, `task_policy_*`, `task_set_exception_ports`,
`host_*` and `semaphore_*` itself, and those wrappers call the
`_kernelrpc_*` client (nextbsd-userland #84 / #93). `UserPrefix` changes only
client symbol names; the generated server files are byte-identical with or
without it.

A new `.defs` that libmach also wraps needs the same prefix. `mach_vm.defs`,
`vm_map.defs` and `clock.defs` deliberately have none (see the comment in
each).

### A `.defs` change crosses repos the moment it merges

Userland CI generates its `mach_port` client from **this repo's `main`**
`.defs` (`nextbsd-userland/.github/workflows/build-arch.yml:72-77`,
`build-userland.sh:262-305`). Anything crossing the kernel/userland boundary
lands in this order:

1. Kernel change, backward-compatible: published userland must still boot on it.
2. Kernel publishes.
3. Userland change consuming it.
4. Userland publishes.
5. Kernel cleanup (for example, removing a trap userland no longer calls).

## Never edit a `*_server.c`

The rule in `build.yml`: **"No generated source is ever committed to this
repo."** The six 2015 servers are still checked in, at
`src-overlay/sys/compat/mach/{clock,host_priv,mach_host,mach_vm,task,vm_map}_server.c`
(21,735 lines). They are dead: `gen_one` overwrites each one at build time.
Edits to them do nothing. Edit the `.defs`. (`mach_port_server.c` is already
gone from the tree; that is the target state.)

## Adding a trap (only after the gate)

**`osx_syscalls[]` in `mach_module.c:230` registers nothing.** `compat_shim.h`
defines every `SYS_*` as `-1` (`NO_SYSCALL`, `:74-159`), which
`kern_syscall_helper_register` treats as the end sentinel. Adding an entry
there silently does nothing. Real wiring is `kern_syscall_register()` in
`mach_syscall_wire.c`, which lets the kernel pick a slot and publishes it as
`sysctl mach.syscall.<name>`. libmach's `resolve_syscall()` reads that sysctl
(`nextbsd-userland/src/libmach/mach_traps.c:78-92`).

1. **Args struct** in `sys/mach/_mach_sysproto.h`, using `PADL_/PADR_` on
   every field. Bare fields silently corrupt arguments. Zero-arg traps use
   `syscallarg_t dummy;`. Six arguments at most.
2. **Include order.** `<sys/sysproto.h>` **must** come before
   `<sys/mach/_mach_sysproto.h>` in every `.c` (`mach_syscall_wire.c:35-38`,
   `_mach_sysproto.h:23-30`).
3. **Handler**: return `0` and put the `kern_return_t` in `td->td_retval[0]`.
   Non-zero only for a real errno (a failed `copyin`/`copyout`). A new `.c`
   file needs a line in `src-overlay/conf/files.compat_mach` (alphabetical,
   same form).
4. **In `mach_syscall_wire.c`**: forward decl, `sys_<name>` prototype,
   `_guarded` wrapper (copy the closest existing shape, e.g. one that calls
   `mach_task_init_lazy()`), a `struct sysent`, the `_offset` and
   `_old_sysent` statics, and `SYSCTL_INT(_mach_syscall, OID_AUTO, <name>, …)`.
5. **Register and deregister**: `wire_one()` appended last in
   `mach_syscall_wire_register()`, `unwire_one()` prepended first in
   `mach_syscall_wire_deregister()`. One list is the exact reverse of the other.

The sysctl uses the userland name (`mach_port_move_member`); the kernel symbol
keeps Apple's `_kernelrpc_*_trap` form. Check for an existing handler first:
`_kernelrpc_mach_port_mod_refs_trap` and `_kernelrpc_mach_port_insert_member_trap`
exist in `mach_traps.c` and are not wired.

## Provenance: check before editing

- **OSF/CMU Mach** (`compat/mach/ipc/`, `compat/mach/kern/`) and **Apple/XNU**
  (`sys/sys/mach/*.h`, `sys/apple/`): vendored. Edit in place and leave a
  `nextbsd#NNN` comment.
- **NextBSD 2014-15** (`mach_traps.c`, `mach_task.c`, `mach_vm.c`, …): ours.
- **MIG output** (`*_server.c`): generated at build time. See above.

## Building, testing, publishing

There is no hot-reload loop. Mach is compiled into the kernel
(`options COMPAT_MACH`); there is no `mach.ko`. Every change is a full kernel
build and a reboot.

**Open a PR.** Jobs: `kernel` (amd64 + arm64 matrix), then `smoke-test` and
`smoke-test-arm64`, which run **only on `pull_request`**, and `publish`
(main only). Every run uploads `nextbsd-kernel-<arch>`; a PR also uploads
`nextbsd-ci-image-<arch>`, a bootable image. Before installing a test kernel on a VM, copy the
current one to `/boot/kernel.old/` first: images ship without a fallback.
`sysctl mach.test_port_lifecycle` and `mach.test_in_kernel_mqueue`
(`mach_test.c`) are in every kernel if you want a quick IPC check by hand.

**The gate is the existing CI, as-is**: `build.yml` plus the pinned
nextbsd-ci boot smoke test, on both arches. Do not add tests, markers,
hardware gates or harness changes to get a change in. The harness is
nextbsd/nextbsd-ci pinned at `v0.3.5` (`build.yml:526`, `671`, `765`); never
modify it from here.

`publish continuous` (`build.yml:807`) needs only the `kernel` job, so both
arches are load-bearing even for an amd64-only change. Nothing downstream
picks it up automatically: nextbsd-pkg publishes only by `workflow_dispatch`.

### Green CI is not evidence

Read the right step, and know what it covered.

- The boot steps carry `continue-on-error: true` (`build.yml:683`, `:770`).
  That masks the **step** conclusion as well as the job's, so a boot that
  exited non-zero still shows a checkmark. Each is followed by **"Gate —
  kernel booted clean"** keyed on `steps.boottest.outputs.rc`
  (`build.yml:700-706`, `785-791`), which prints `BOOT-GATE-FAIL` and the
  harness rc. Read the Gate step and grep the uploaded `boot-log` /
  `boot-log-arm64` transcript for markers; do not read the checkmark.
- The kernel smoke tests are **login-only** (`NB_LOGIN_ONLY=1`), under qemu,
  against published userland. No on-image Mach suite runs in kernel CI.
- Mach regressions show up in **nextbsd-userland's** lanes, which are gating
  (`boot_soft: false`) and run the full on-image suite against this repo's
  published `continuous` kernel. That is after merge.
- Green on qemu is necessary, not sufficient.

## Before you open the PR

- Does Apple reach this by MIG? Does a `.defs` here already cover it? Then no trap.
- Is the handler you are calling actually implemented, or `UNSUPPORTED`?
- Did a `.defs` change move any `msgh_id`? `.msgids` updated deliberately, or not at all?
- Did both `kernel` legs compile, not just the ABI gate pass?
- Does it touch the userland boundary? Then kernel first, backward-compatible.
- Did you edit a checked-in `*_server.c`? That edit does nothing.
