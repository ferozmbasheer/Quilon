# Quilon Architecture

A high-level map of the system, intended as an onboarding aid. Where exact
addresses are given, treat the linker scripts and headers as authoritative —
they are the source of truth and this document should be kept in sync with them.

> Status: initial draft. Verify addresses against `kernel/arch/i386/linker.ld`,
> `user/link.ld`, and the relevant headers before relying on them.

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

## Processes, threads, scheduling

- `process.c` owns the process table and `process_t` (address space / `cr3`,
  kernel stack, VMAs, heap end, state, parent pid, name).
- `scheduler.c` is a round-robin preemptive scheduler driven by the PIT.
- `fork` uses copy-on-write (`paging.c` marks PTEs COW and the fault handler
  copies on write).
- Threads share an address space via `clone(CLONE_VM)` — this is how the window
  manager runs its apps as threads in one process; the user-side `pthread`
  wrapper lives in `user/libc/pthread.c`.

## Syscalls

Dispatched through a single switch in `kernel/kernel/syscall.c`. The register
convention is documented in [BUILD.md](BUILD.md); user-side stubs are in
`user/libc/syscall.S`.

> Hardening note: most syscalls currently dereference user pointers directly. A
> `copyin`/`copyout`/`access_ok` layer (validating against the process VMA table
> via `vma_find`) is the planned next robustness step; new syscalls that take
> user pointers should adopt it.

## Filesystem

`vfs.c` provides a function-pointer VFS over multiple backends: `fat16.c` (the
on-disk FAT16 image, `disk.img`), `initrd.c` (the boot module), and `pipe.c`
(anonymous pipes). `elf.c` loads ELF executables into a new address space.

## Graphics & windowing (user space)

- `user/libgfx` — 2D canvas abstraction (32-bit ARGB), drawing primitives
  (rects, lines via Bresenham, text, blits with clipping via `pixel_visible`).
- `user/wm` — window manager + compositor. Windows occupy fixed slots with a
  separate `z_order[]` index (so `window_t*` pointers stay stable across raise),
  events flow to clients through per-window SPSC ring buffers, and the
  compositor (`wm_compositor.c`) draws chrome + blits client back-buffers.
- Apps (`gterm`, `clock`, `filebr`, `textview`, …) run as `CLONE_VM` threads of
  the WM process and draw into their own back-buffer canvases.

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
