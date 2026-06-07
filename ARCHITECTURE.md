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

## Processes, threads, scheduling

- `process.c` owns the process table and `process_t` (address space / `cr3`,
  kernel stack, VMAs, heap end, state, parent pid, name).
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
> stdin/stdout fds).

## Filesystem

`vfs.c` provides a function-pointer VFS over multiple backends: `fat16.c` (the
on-disk FAT16 image, `disk.img`), `initrd.c` (the boot module), and `pipe.c`
(anonymous pipes). `elf.c` loads ELF executables into a new address space.

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
- The kernel tracks the single graphics owner (`gfx_owner_pid`, syscall.c):
  refuses a second WM, and while graphics mode is active suppresses the kernel
  VBE text console + kernel mouse cursor (`vbe_set_graphics_mode`) so they don't
  flash on the composited desktop.

The WM logic in `wm.h` is written as testable `static inline` functions and
exercised by `tests/test_wm.c` on the host.

## Networking

A TCP/IP stack exists in the kernel (`kernel/kernel/net.c` over the RTL8139
driver, `arch/i386/rtl8139.c`) but is **not yet exposed to user space** — adding
the BSD socket syscalls is the next roadmap milestone (Section 15).

## Testing

All unit tests are **host-compiled** (`tests/`, custom `framework.h`): kernel
`.c` files are built with the host gcc, with hardware-only code excluded via
`#ifdef __is_kernel`. There is currently no in-VM integration test beyond the
`make smoke` headless-boot check.
