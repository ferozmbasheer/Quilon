# Quilon — guide for Claude Code

Quilon is an educational i386 (x86-32) operating system. Read
[ARCHITECTURE.md](ARCHITECTURE.md) for the system map and [BUILD.md](BUILD.md)
for toolchain details.

## Commands

```sh
make          # build kernel + libc (needs i686-elf cross toolchain)
make test     # host unit-test suite (host gcc only) — run on every change
make run      # boot under QEMU (GUI)
make smoke    # headless QEMU boot smoke test
make clean
```

`make` wraps the shell scripts (`config.sh` → `headers.sh`/`build.sh` →
`iso.sh` → `qemu.sh`), which remain the source of truth.

## Layout

- `kernel/arch/i386/` — boot, GDT/IDT, paging, PMM, PIT, SMP/APIC, drivers
  (ata, mouse, rtl8139, pci, vbe, psf, tty, serial).
- `kernel/kernel/` — portable kernel: scheduler, process, syscall, signal, vfs,
  fat16, elf, initrd, pipe, vma, net, kmalloc, keyboard, and the in-kernel shell.
- `kernel/include/kernel/` — public kernel headers.
- `libc/` — freestanding libc (built as `libk.a` for the kernel).
- `user/` — ring-3: `libc`, `libgfx`, `wm` (+ compositor), `shell`, and apps.
- `tests/` — host unit tests + `framework.h`.
- `tools/` — disk/initrd builders, font converter, `smoke_test.sh`,
  `new_test.sh`.

## Conventions to respect

- **Dual shell:** changes to shell commands must be made in BOTH
  `kernel/kernel/shell.c` and `user/shell/main.c`.
- **Validate user pointers** in any new syscall: use the existing
  `uap_ok` / `uap_str_ok` / `copyout` helpers in `kernel/kernel/syscall.c`
  (they check the range against the process VMA table). Do not dereference raw
  user pointers.
- **IRQ-safe locks:** a lock or state shared between thread and interrupt/
  exception context MUST disable interrupts while held
  (`spinlock_acquire_irqsave`). This was the source of multiple hard freezes.
- **Tests are host-compiled.** Add one for new kernel logic
  (`tools/new_test.sh <name>`, then wire into `tests/Makefile`). Guard
  hardware-only code with `#ifdef __is_kernel`.
- **Don't commit build artifacts** — `.gitignore` already covers them.

## Build gotcha (important)

`make disk` rebuilds **only** the userspace disk image (`disk.img`); it does
**not** rebuild the kernel ISO. To test a **kernel** change you must run
`./iso.sh` (or top-level `make`/`make run`, which chain it). Booting after a
bare `make disk` runs a stale kernel — this wasted real debugging time.

## Roadmap / progress

Current status and plan: [ROADMAP3.md](ROADMAP3.md). **Done through Section 14
(graphical desktop, stable);** next is Section 15 (BSD socket API — the kernel
TCP/IP stack in `net.c` exists but isn't exposed to user space yet). The WM was
reworked from CLONE_VM threads to a **single-threaded event loop** (apps =
callbacks) — see `resume.md` and the "WM architecture" / "thread concurrency"
project memories.

A longer-form project memory lives under
`.claude/projects/.../memory/` (indexed by `MEMORY.md`). **`resume.md`** at the
repo root is the handoff doc for continuing after a context reset.
