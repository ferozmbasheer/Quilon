# Quilon — Architecture & Stability Review (2026-11)

A review of the kernel after Section 16, before starting the Section 17 ports.
It covers the riskiest paths — the syscall boundary, page-fault handling, the
VFS / fd table, and process lifecycle — not every line of the ~39k-line tree.
Findings came from code reading and were then reproduced; each fix lands with
a host unit test and/or an in-OS regression check (see *Testing* below), and
each in-OS check was shown to FAIL on the unfixed code.

Status legend: **open** / **fixed** (with a summary of the fix).

---

## Stability

### A. Kernel panics when a syscall writes to a not-yet-touched heap page — *fixed*

The heap is demand-paged: `SYS_SBRK` only extends the heap VMA; a physical page
is mapped on first touch by the page-fault handler. But the handler only
services **user-mode** faults (`exceptions.c`, `if ((regs->cs & 3) == 3)`); a
kernel-mode fault falls through to `KERNEL PANIC`.

So `buf = malloc(64*1024); read(fd, buf, 64*1024);` passes `uap_ok` (the range
is inside the heap VMA) and then panics when `vfs_read` writes the second page.
Today's programs dodge it (stack buffers, small allocations); Lua's `fread`
into a large buffer or an editor loading a file will not.

**Fixed:** the fault decision is now a pure function, `vma_fault_action()`
(`vma.c`), which services ring-0 faults on *user* addresses inside a VMA exactly
like ring-3 ones; kernel addresses, NULL and out-of-VMA ring-0 faults still
panic. Reproduced first: `ktest` panicked the old kernel on its first check.
*Tests:* `test_vma` (`test_pf_*`), `ktest` "read() into untouched heap pages".

### B. Kernel writes bypass copy-on-write (CR0.WP not set) — *fixed*

CR0.WP (bit 16) is never set on the BSP (`paging.c`) or the APs
(`smp_trampoline.S`). Without it, ring 0 ignores read-only PTEs, so a `read()`
/ `copyout` into a CoW-shared page after `fork` writes the **shared** physical
frame and the data leaks into the other process.

**Fixed:** CR0.WP is set on the BSP and the APs; the ring-0 CoW fault goes
through `paging_cow_handle` (via A). Safe because every kernel mapping and every
user PTE is writable at the PTE level (permissions live in the VMAs).
*Tests:* `test_vma` (`test_pf_kernel_cow`), `ktest` "kernel write breaks CoW
after fork" (fails with WP cleared, A kept).

### C. Exited processes never free their address space — *fixed*

`SYS_EXIT` → `PROC_ZOMBIE` → `SYS_WAIT` → `PROC_UNUSED` only flips states.
Nothing frees the page directory, page tables, user frames or CoW references.
Every `exec` leaks its whole address space; a long desktop session eventually
runs out of physical memory.

**Fixed:** `process_reap()` frees the slot and calls
`paging_destroy_address_space()` (a pure walk, `as_destroy()` in
`arch/i386/addrspace.c`) unless a CLONE_VM sibling still uses that CR3. A new
`PAGE_NOFREE` PTE bit marks the WM's mapping of the kernel's VBE shadow buffer
so teardown (and fork) never free/refcount it. Exec failure paths now free a
partially loaded address space; `pmm_ref_page` takes the PMM lock.
*Tests:* `test_addrspace` (new), `test_process` lifecycle suites, `ktest`
fork/exec/SIGSEGV leak checks (0 pages leaked; 15–30 before) and the
framebuffer-crash check.

### D. Death by signal skips the exit cleanup — *fixed*

Exit logic is copy-pasted in three places: `SYS_EXIT`, signal SIG_DFL, and the
not-yet-supported user-handler path. Only `SYS_EXIT` closes owned pipe fds and
releases the graphics surface. If the WM dies with SIGSEGV, graphics mode and
`gfx_owner_pid` stay held, so `startx` can never reclaim the screen.

**Fixed:** `process_exit()` / `process_mark_exited()` in `process.c` is the
single exit path for `SYS_EXIT` and both signal paths: closes owned pipe fds,
releases the graphics surface (`gfx_release_owner`), reaps/orphans children,
wakes the parent. *Tests:* `test_process`, `ktest` "graphics surface released
after the owner crashed" (uses a different pid than the crasher, since pids are
slot numbers and reuse would mask the bug).

### E. fd and process-slot leaks — *partly fixed*

- The fd table is global (32 slots). Exit closes only the stdin/stdout pipe
  overrides; any other file a process leaves open is leaked forever. Any
  process can also read or close another process's fd by number.
- Orphans are never reparented, and `gterm_app.c` intentionally does not
  `wait()` its shell, so each closed terminal leaves a zombie behind.

**Fixed so far:**
- Orphans are reparented to `PROC_NO_PARENT` and reaped automatically
  (`process_reap_orphans`, run before every slot allocation); a dying parent
  reaps its zombie children.
- `SYS_WAIT` gained `WAIT_NOHANG` (`wait_nohang()` in libc) and only lets a
  parent (or its thread group) reap a child. The WM reaps closed terminals'
  shells from its loop (`gterm_reap_shells`).
- Found while fixing this: the ring-3 shell retried `read()` forever on EOF, so
  **every closed terminal left a shell spinning at 100% CPU**; it now exits.
  And `gterm_destroy` closed the shell's own pipe ends, a double close of a
  global fd slot another process may have reused; it now closes only its own.
- *Tests:* `test_process`, `ktest` orphan + shell-EOF checks, `make wmtest`
  (open/close a terminal 3x; on the old code no shell ever exited).

**Still open:** fds other than the stdin/stdout overrides are not closed on
exit, and any process can use another's fd number — needs per-fd ownership or
per-process fd tables (see Architecture).

### H. Every exec'd process could read and write kernel memory — *fixed*

*(Found during the fixes; the most serious finding.)* `usermode_initialize()`,
run by `process_launch` for every exec'd process, set `PAGE_USER` on the
identity map **and** the kernel-high window — they share one page table — so
the shell, the WM and every app could read and write the kernel image, PCBs
and page tables. A stray app pointer silently corrupted the kernel instead of
raising SIGSEGV. It existed for the in-kernel ring-3 teaching demos, whose code
lives in kernel text.

**Fixed:** `usermode_initialize()` now only loads the TSS. The demos call
`usermode_grant_demo_access()` explicitly and `shell_run_ring3_task` revokes it
on the kernel PD afterwards (`paging_clear_user_access`), so later address
spaces don't inherit it. *Tests:* `ktest` exec's `kprobe.elf` (must be exec'd —
a forked child never had the grant) against kernel text and the identity map;
both reads succeeded before. Kernel-shell `sbrk`/`fork` demos verified on a
blank disk.

### F. No locking in the VFS or filesystem drivers — *fixed*

Only the BSP schedules, but syscalls run with IRQs enabled (trap gate) and are
preemptible. Two processes in `vfs_open` can claim the same fd slot, and the
FAT16 / ext2 drivers share static scratch buffers (`sector_buf`, `meta_buf`,
…), so a Files-app listing racing a terminal `cat` can corrupt either read.

Reproduced: four processes reading *different* files concurrently got wrong
data in 1–3 of 600 reads on both FAT16 and ext2. (Reading the same file hides
the fd-slot race — swapped slots return identical bytes.)

**Fixed:** one VFS lock in `vfs.c` covers the fd table, the working directory
and every driver call; it is never held across pipe/socket I/O, which blocks.
A waiter marks itself READY and yields to the (preempted) holder instead of
spinning — FAT16 stress throughput went up ~3x as a side effect, since waiters
no longer burn their slices spinning on the ATA lock. *Tests:* `test_vfs` lock
suites (a spy driver asserts the lock is held in every callback and released
on every success/error path), `ktest` "concurrent opens/reads of different
files" (0 of 600 wrong, 8/8 parallel runs).

### I. wait() could hang forever when another child exited first — *fixed*

*(Found by F's stress check, which hung ~1 run in 4.)* A parent in `SYS_WAIT`
is woken by **any** child's exit, but it set `PROC_BLOCKED` only once, before
its loop. After a sibling's exit woke it, it yielded again while still marked
RUNNING; `scheduler_yield` left that state alone and `process_pick_next` only
picks READY processes, and the awaited child's exit only wakes a BLOCKED
parent. The parent was never scheduled again. (Likely also behind the one
unexplained `startx` failure seen early on.)

**Fixed:** `SYS_WAIT` re-blocks on every iteration *before* checking the
child, and `scheduler_yield` turns a still-RUNNING caller into READY, so a
voluntary yield can never strand a process. *Tests:* `ktest` "wait() for one
child survives a sibling exiting first" — deterministic; hangs the old kernel.

### G. Kernel stack overflow went undetected — *fixed (guard); size kept*

`kernel_stack[4096]` lives inside the PCB. The stack grows down, so an overflow
silently overwrites the fields declared *before* it — `name`, the signal
handlers, the std fds, `heap_end`, `cr3`, `state` — and then the previous
PCB's VMA table. (The original note said "the VMA table after it"; wrong
direction.)

**Fixed:** a canary at the low end of every kernel stack is checked on each
context switch (`scheduler_check_stack` panics naming the pid), and exit warns
when a process used more than 3/4 of its stack. The stack is pattern-filled at
creation so `process_stack_used()` reports a real high-water mark.

**Size kept at 4 KiB, on evidence:** the measured high-water across `ktest`
(including ring-0 page faults inside syscalls, concurrent file I/O, fork/exec,
signals) and the desktop with terminals was ~1 KiB, and the largest stack
locals reachable from a syscall are ~280 bytes (network buffers are static).
*Tests:* `test_process` stack-guard suites.

---

## Architecture

- **Per-process fd tables** — per-process fd → global open-file-description
  table with refcounts. The real fix for E; removes the
  `stdin_fd`/`stdout_fd`/`owns_std_fds` workaround and gives ported programs
  real fd inheritance and `dup2`.
- **Per-process working directory** — `vfs_cwd` is one global: `cd` in one
  terminal changes the directory of every process (found while adding F).
- **errno** — every syscall returns a bare `-1`. Return `-errno` and add an
  `errno` to user libc before Lua / TinyCC start checking it.
- **Retire the dual shell** — the kernel shell (~1.5k lines) and ring-3 shell
  (~1.4k lines) are hand-synced. The ring-3 shell is mature; the kernel shell
  could shrink to a minimal recovery shell.

## Polish

- *Done:* the stale "pointer is trusted / first 4 MiB is user-accessible"
  notes on `SYS_WRITE` / `SYS_OPEN` now describe the `uap_ok` validation.
- *Done:* `[kernel] pid N exited` goes to the serial log only, not into the
  user's terminal after every command.
- *Done:* `make smoke` requires the ring-3 shell banner by default
  (`SMOKE_SENTINEL=''` disables it), so a boot that hangs fails; a failing
  run keeps its full serial log in `smoke-fail.log`.
- *Open:* `kernel.c` is mostly per-section boot demos; gate them behind a flag
  for a quicker, readable boot.
- *Open:* the network stack has one global TCP connection and shared static
  frame buffers (`g_rx_buf` / `g_tx_buf`), so two processes using sockets at
  once would interfere — the same class of problem as F, for when sockets get
  more use.

## Testing

Host unit tests (`make test`) cover the pure-C policy code. The hardware paths
are covered by new headless in-OS checks:

- **`make ktest`** boots QEMU, types `exec ktest.elf` into the ring-3 shell via
  the QEMU monitor (`tools/qemu_type.py`) and requires `KTEST: ALL PASS`.
  `user/ktest` holds one check per bug, each verified to fail on the unfixed
  kernel (by reverting / mutating the fix). Run it on both images:
  `make ktest` and `make ext2disk && SMOKE_DISK=ext2.img make ktest`.
- **`make wmtest`** runs `startx` and opens/closes a Terminal three times with
  the mouse, requiring three shell exits.
- `tools/smoke_test.sh` gained `SMOKE_KEYS` (script typed/clicked into the VM,
  with `{mouse:dx,dy}`, `{click}`, `{wait:s}`, `{until:text}`) and
  `SMOKE_SENTINEL_COUNT`; it stops as soon as the sentinel (or a crash
  marker) appears instead of always waiting out the timeout (a `ktest` run
  went from 180 s to ~20 s), and a failing run keeps its full serial log in
  `smoke-fail.log` (gitignored).
- Of ~40 `ktest` runs after the fixes, one ext2 run failed early on without
  a kept log (before `smoke-fail.log` existed) and never reproduced in 13
  later runs; if it recurs, the kept log will say why.

## Order of work

A + B first (Section 17 ports will hit them immediately), then C + D as one
`process_exit` change, then F, then E, then G and polish.

Done: A, B, C, D, F, G (guard), H, I, the process half of E, and three of
the polish items. Still open: the fd half of E (per-process fd ownership),
the Architecture items, boot-demo gating, and the network-stack note.
