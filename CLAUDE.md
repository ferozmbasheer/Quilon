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
make ktest    # headless in-OS kernel regression tests (user/ktest) — run on kernel changes
make wmtest   # headless desktop test: open/close terminals via the mouse, then
              #   resize / maximize / restore / minimize + Alt-Tab restore (18.3)
make kbdtest  # headless keyboard test: shift, arrows, Ctrl-C (text + desktop)
make termtest # headless: `exit` in a terminal window closes it (nested shell doesn't)
make eventtest # headless 18.2 events: fast click, Alt-Tab, Alt-F4, wheel present
make powertest # headless: shell `poweroff` and the desktop Power button stop QEMU
make disk     # rebuild the FAT16 disk.img
make ext2disk # rebuild ext2.img (needs host mke2fs)
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

Current status and plan: [ROADMAP3.md](ROADMAP3.md). **Done through Section 16**
(graphical desktop stable; BSD sockets + DNS + wget; ATA Bus Master DMA; ext2
read-only, booting off an ext2 disk); next is Section 17 (porting Lua, a text
editor, TinyCC), starting with the new 17.0 prerequisites (17.0.1 keyboard + Ctrl-C is done)
and interleaved with Section 18 (desktop polish; 18.1 idle, damage-driven WM loop,
18.2 richer input events and 18.3 window operations are done; next 18.4
taskbar window list + tray). The WM was reworked from CLONE_VM threads to a
**single-threaded event loop** (apps = callbacks) — see `resume.md` and the
"WM architecture" / "thread concurrency" project memories.

A pre-§17 stability review is tracked in [REVIEW.md](REVIEW.md) (items A–J
fixed -- J was `context_switch` not saving EFLAGS; per-process fd tables/cwd,
errno and a few others still open). Keep its
statuses current when touching those areas, and add a `user/ktest` check
(shown to fail without the fix) for any new kernel bug fix.

Two filesystems are mounted through the same VFS: the kernel probes **ext2**
first and falls back to **FAT16**. `make disk` builds the FAT16 `disk.img`;
`make ext2disk` builds `ext2.img` (needs host `mke2fs`).
`VFS_NAME_MAX` (kernel) and `DIRENT_NAME_MAX` (user libc) are a shared ABI and
must stay equal.

A longer-form project memory lives under
`.claude/projects/.../memory/` (indexed by `MEMORY.md`). **`resume.md`** at the
repo root is the handoff doc for continuing after a context reset.
