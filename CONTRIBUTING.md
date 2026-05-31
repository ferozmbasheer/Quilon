# Contributing to Quilon

This is a personal learning OS, but the workflow below keeps changes consistent
and testable. See [ARCHITECTURE.md](ARCHITECTURE.md) for how the pieces fit
together.

## Build & test

```sh
make          # build kernel + libc (needs the i686-elf cross toolchain)
make test     # host unit tests (host gcc only — run this on every change)
make run      # boot under QEMU
make smoke    # headless boot smoke test
```

Always run `make test` before considering a change done. Most kernel logic has a
host-compiled unit test (the kernel `.c` files are compiled with the host gcc and
hardware-only code is guarded by `#ifdef __is_kernel`).

## Conventions

- **Match the surrounding style.** The codebase favors C89/C11-ish kernel C with
  block comments describing each subsystem and syscall.
- **Shell changes go in two places.** There are two shells: the in-kernel debug
  shell `kernel/kernel/shell.c` and the ring-3 shell `user/shell/main.c`. A
  command change generally has to be made in **both**.
- **No new build artifacts in git.** `.gitignore` already covers `*.o`, `*.a`,
  `*.elf` (except the demo `hello.elf`), `*.iso`, `*.img`, `sysroot/`, `isodir/`,
  `build/`, and `*.orig`.

## Adding a syscall

1. Add the syscall number to the syscall enum/header used by both kernel and
   user side (see existing `SYS_*` definitions).
2. Add a `case SYS_FOO:` to the dispatch switch in
   `kernel/kernel/syscall.c`. **Validate every user pointer** before
   dereferencing it (today many syscalls dereference raw user pointers — when
   the `copyin`/`copyout`/`access_ok` helpers land, route new syscalls through
   them; until then, follow the most defensive existing case as a model).
3. Add a user-space wrapper in `user/libc/` (and the asm stub in
   `user/libc/syscall.S` following the documented register convention in
   [BUILD.md](BUILD.md)).
4. Add a host unit test (`tools/new_test.sh foo` scaffolds one; then wire it into
   `tests/Makefile`).

## Adding a device driver

Self-contained drivers live in `kernel/arch/i386/` (see `rtl8139.c`, `ata.c`,
`mouse.c` as templates). Register the device, expose a small kernel API in a
header under `kernel/include/kernel/`, and add a host unit test where the logic
is testable without hardware.

## Adding a test

```sh
tools/new_test.sh <name>     # creates tests/test_<name>.c from a template
```

Then add the two printed lines to `tests/Makefile` (a `TESTS` entry and a build
rule). The test list is explicit on purpose — each test links a hand-picked set
of kernel sources.

## Commits

Group changes into logical, reviewable commits. The maintainer reviews and
commits manually — when working as an assistant, make the changes but **do not
commit**; pause for review.
