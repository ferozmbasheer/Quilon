# Quilon Architecture

A high-level map of the system, intended as an onboarding aid. Where exact
addresses are given, treat the linker scripts and headers as authoritative —
they are the source of truth and this document should be kept in sync with them.

> Status: current as of 2026-06 (Sections 11–14 complete, desktop stable).
> Verify addresses against `kernel/arch/i386/linker.ld`, `user/link.ld`, and the
> relevant headers before relying on them.

## Boot flow

1. GRUB loads the multiboot kernel (`quilon.kernel`) and the initrd module
   (`initrd.img`, which carries the PSF2 console font). See `iso.sh` /
   `tools/make_initrd.py`.
2. `kernel/arch/i386/boot.S` sets up the multiboot entry, the initial stack, and
   paging for the higher half, then jumps to the C entry point in
   `kernel/kernel/kernel.c`.
3. The C entry initializes, in order, the descriptor tables (GDT/IDT,
   `gdt.c`/`interrupts.c`), physical memory (`pmm.c`), paging (`paging.c`),
   the heap (`kmalloc.c`), devices (timer/PIT, keyboard, mouse, ATA, PCI,
   RTL8139, VBE), the VFS + FAT16, then starts the scheduler and drops to the
   ring-3 shell.

## Memory layout (per the linker scripts / headers)

| Region                | Address           | Source |
|-----------------------|-------------------|--------|
| Kernel (higher half)  | `0xC0100000`      | `kernel/arch/i386/linker.ld` |
| User program load     | `0x00400000`      | `user/link.ld` |
| User heap start       | `0x00800000`      | `USER_HEAP_START` (`syscall.c`) |
| Mapped screen FB      | `0x60000000`      | `SYS_GFX_MAP` / `user/libgfx` |

The kernel is a higher-half kernel; user space lives in the low canonical range.
Per-process virtual memory is tracked with **VMAs** (`kernel/kernel/vma.c`,
`vma.h`) and pages are **demand-paged** — `sbrk` extends the heap VMA and the
page-fault handler allocates physical pages lazily on first write.

**>4 MiB physical access:** only the first 4 MiB is identity-mapped, so the
kernel cannot dereference an arbitrary physical page directly. Page tables /
directories stay in low identity-mapped memory; *data* pages (heap, ELF
segments, stack, CoW copies) are allocated above 4 MiB via
`pmm_alloc_data_page()` and touched through a transient mapping window,
`kmap()` / `kunmap()` (paging.c). This is what lets the graphical desktop's
working set exceed 4 MiB without faulting the kernel.

**Isolation and faults (REVIEW.md A/B/H).** The identity map and the kernel
half are shared by every address space but are **not** user-accessible; only
the in-kernel ring-3 teaching demos get a temporary grant
(`usermode_grant_demo_access` / `..._revoke_...`). CR0.WP is set on every CPU,
so ring 0 honours read-only PTEs. The page-fault policy is the pure
`vma_fault_action()` (vma.c): a fault on a user address inside a VMA is demand-
paged (not present) or copy-on-write resolved (write to a present page) whether
it came from ring 3 or from ring 0 inside a syscall copying to a user buffer;
anything else is SIGSEGV from ring 3 and a panic from ring 0. PTE bit 10,
`PAGE_NOFREE`, marks kernel memory mapped into a process (the WM's view of the
VBE shadow buffer): fork shares it as-is and teardown skips it.

## Processes, threads, scheduling

- `process.c` owns the process table and `process_t` (address space / `cr3`,
  kernel stack, VMAs, heap end, state, parent pid, name). PIDs are slot
  numbers, so they are reused as soon as a slot is reaped.
- **Lifecycle:** every death (SYS_EXIT, fatal signal) goes through
  `process_exit` → `process_mark_exited` (close owned pipe fds, release the
  graphics surface, reap zombie children / orphan live ones to
  `PROC_NO_PARENT`, wake the parent). `process_reap` (from `wait`, or the
  orphan sweep before each slot allocation) frees the slot and the address
  space via `paging_destroy_address_space` / `as_destroy` — unless a CLONE_VM
  sibling still uses that CR3. `wait` only lets the parent (or its thread
  group) reap, and supports `WAIT_NOHANG`.
- Kernel stacks are 4 KiB inside the PCB with a canary in the lowest word,
  checked on every context switch; exit warns past 3/4 use (measured
  high-water ~1 KiB).
- `scheduler.c` is a round-robin preemptive scheduler driven by the PIT.
- `fork` uses copy-on-write (`paging.c` marks PTEs COW and the fault handler
  copies on write).
- Threads share an address space via `clone(CLONE_VM)` (user-side `pthread`
  wrapper in `user/libc/pthread.c`). **The WM no longer uses this** — it was the
  source of severe races and was replaced by a single-threaded design (see
  Graphics & windowing below). `clone` remains available but is not used by the
  shipped GUI.

**Concurrency hardening (learned the hard way):** any lock or piece of state
shared between thread context and interrupt/exception context must disable
interrupts while held — the IRQ stubs (`boot.S`) load the kernel data segment,
and `pmm_lock`/`tty_lock`/`kmap_lock`/`waitq` are IRQ-safe (`spinlock.h`
`spinlock_acquire_irqsave`). ATA PIO is serialized with a plain spinlock (it is
slow and never called from IRQ context). See the "thread concurrency" project
memory for the specific bugs these prevent.

Syscalls are preemptible (int $0x80 is a trap gate and IRQ0 switches tasks even
in ring 0). The VFS therefore has its own lock (`vfs.c`) around the fd table,
the cwd and every driver call; a waiter marks itself READY and yields to the
holder rather than spinning. `scheduler_yield` turns a still-RUNNING caller
into READY, and code that sleeps must set BLOCKED *before* re-checking its
condition (the `wait()` lost-wakeup bug, REVIEW.md I).

## Syscalls

Dispatched through a single switch in `kernel/kernel/syscall.c`. The register
convention is documented in [BUILD.md](BUILD.md); user-side stubs are in
`user/libc/syscall.S`.

> Hardening note: user pointers ARE now validated. `uap_ok`/`uap_str_ok` /
> `copyout` (syscall.c) check a range against the process's VMA table
> (`vma_range_ok`) before the kernel dereferences it; new syscalls that take user
> pointers must use them. `sbrk` is bounds-checked; `SYS_WAIT` re-resolves the
> child each wake. Notable newer syscalls: `SYS_READ_NB` (35, non-blocking read,
> used by the WM loop) and `SYS_EXEC_REDIR` (36, exec with explicit child
> stdin/stdout fds). Since the 2026-11 review: `SYS_WAIT` takes an options
> word in EDX (`WAIT_NOHANG`; libc `wait_nohang()`), and `SYS_MEMINFO` (53)
> returns the free physical page count (used by ktest's leak checks).

## Filesystem

`vfs.c` provides a function-pointer VFS over multiple backends: `ext2.c`
(read-only, probed first) or `fat16.c` (`disk.img`) on the disk, `initrd.c`
(the boot module), and `pipe.c` (anonymous pipes); sockets also live in the fd
table. Paths are normalised against the CWD (`vfs_normalize`). The fd table
(32 slots) and the CWD are **global**, not per-process — see REVIEW.md E and
Architecture. `elf.c` loads ELF executables into a new address space.

## Graphics & windowing (user space)

The WM is **single-threaded**: one process, one event loop, apps as
non-blocking callbacks. Only the loop touches WM state and the framebuffer, so
there are no shared-state races (this replaced an earlier `CLONE_VM`-thread
design that was deeply race-prone).

- `user/libgfx` — 2D canvas abstraction (32-bit ARGB), drawing primitives
  (rects, lines via Bresenham, text, blits with clipping via `pixel_visible`).
- `user/wm` — window manager + compositor. Windows occupy fixed slots with a
  separate `z_order[]` index. Each `window_t` holds `app_state` + callbacks
  (`on_event`, `on_tick`, `on_destroy`) instead of a thread/event-queue. The
  loop: poll mouse → poll keyboard (`read_nonblock`) → tick windows → destroy
  windows that set `want_close` → composite (`wm_compositor.c`) + taskbar +
  cursor-last + `gfx_flush`.
- Apps (`gterm`, `clock`, `filebr`, `textview`) are launched via `*_open(win)`
  entry points that install callbacks and allocate `app_state`. The terminal
  drains its shell's stdout pipe in `on_tick` (non-blocking) and spawns the
  shell with `exec_redir` so the WM keeps reading the real keyboard.
- **Keyboard and Ctrl-C (ROADMAP3 17.0.1).** `kbd_xlate.c` turns scancodes into
  bytes (shifted layout, Ctrl codes, VT100 escape sequences for arrows/F-keys).
  Ctrl-C is a SIGINT to a shell's *foreground job*: shells `setfg(child)`
  around `wait()`, and the kernel follows the `fg_child` chain
  (`process_intr_target`). On the text console the keyboard IRQ does this from
  `console_shell_pid`; on the desktop the WM ignores SIGINT, receives 0x03 as a
  key, and gterm calls `intr(shell_pid)`. Signals are dispatched at syscall
  exit and on timer-IRQ return to ring 3; blocking reads and `wait()` abort
  when one is pending.
- The kernel tracks the single graphics owner (`gfx_owner_pid`, syscall.c):
  refuses a second WM, and while graphics mode is active suppresses the kernel
  VBE text console + kernel mouse cursor (`vbe_set_graphics_mode`) so they don't
  flash on the composited desktop.

The WM logic in `wm.h` is written as testable `static inline` functions and
exercised by `tests/test_wm.c` on the host.

## Networking

A TCP/IP stack in the kernel (`kernel/kernel/net.c` over the RTL8139 driver,
`arch/i386/rtl8139.c`) is exposed to user space through BSD-style socket
syscalls (44–50) and `SYS_DNS_RESOLVE` (51); `wget` fetches real pages.
Networking is polled, not IRQ-driven, and there is a single global TCP
connection with shared static frame buffers — two processes using sockets at
once would interfere.

## Testing

Unit tests are **host-compiled** (`tests/`, custom `framework.h`): kernel
`.c` files are built with the host gcc, with hardware-only code excluded via
`#ifdef __is_kernel`. Keep policy logic pure so it can be tested this way
(e.g. `vma_fault_action`, `as_destroy`, the process lifecycle).

In-VM tests run headless under QEMU (`tools/smoke_test.sh`):
- `make smoke` — boots and requires the ring-3 shell banner.
- `make ktest` — types `exec ktest.elf` into the shell through the QEMU
  monitor (`tools/qemu_type.py`); `user/ktest` has one check per fixed kernel
  bug and must print `KTEST: ALL PASS`. Run on both images
  (`SMOKE_DISK=ext2.img make ktest`).
- `make wmtest` — `startx`, then opens/closes a Terminal three times with the
  mouse and counts shell exits.
A failing run keeps its serial log in `smoke-fail.log`.
