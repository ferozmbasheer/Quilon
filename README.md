# Quilon

\#experiments \#learning-os \#learning-fundamentals

A hobby operating system for the i386 (x86-32) architecture, built for learning
OS fundamentals from the ground up — bootstrapping, paging, preemptive
scheduling, a VFS with FAT16, ELF loading, a ring-3 user space with a small
libc, a 2D graphics library, and a window manager with compositor.

Based on resources from the [OSDev wiki](https://wiki.osdev.org) and other
sources.

## Status

Complete through **Section 14.4 — Simple GUI Apps** (window manager, compositor,
graphical terminal, and several apps). Next milestone: **Section 15 — BSD Socket
API for user space** (the TCP/IP stack already exists in the kernel; it just
isn't exposed to user programs yet). See [ROADMAP3.md](ROADMAP3.md).

## Quick start

Requires an `i686-elf` cross toolchain, GRUB, QEMU, and `python3` (see
[BUILD.md](BUILD.md) for the full dependency list and how to build the
toolchain).

```sh
make          # build the kernel + libc
make run      # build everything and boot under QEMU
make test     # run the host unit-test suite (needs only host gcc, no toolchain)
make smoke    # headless QEMU boot smoke test
make clean    # remove build artifacts
```

`make` is a thin wrapper over the existing shell scripts (`build.sh`, `iso.sh`,
`qemu.sh`, …), which remain the source of truth.

## Documentation

- [BUILD.md](BUILD.md) — dependencies, toolchain, build/run, syscall convention
- [ARCHITECTURE.md](ARCHITECTURE.md) — memory layout, boot/trap flow, subsystems
- [CONTRIBUTING.md](CONTRIBUTING.md) — how to add a syscall, driver, or test
- [ROADMAP3.md](ROADMAP3.md) — current roadmap and learning path

## Layout

```
kernel/   kernel sources (arch/i386 + portable kernel/) and public headers
libc/     freestanding C library (built as libk.a for the kernel)
user/     ring-3 programs: libc, libgfx, wm (+ compositor), shell, apps
tests/    host-compiled unit tests (custom framework in framework.h)
tools/    disk/initrd builders, font converter, smoke test, test scaffold
```
