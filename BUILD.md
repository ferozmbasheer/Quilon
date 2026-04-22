# Building and Running Quilon

Quilon is a bare-metal hobby OS targeting 32-bit x86. It requires a cross-compiler toolchain because host system headers and libraries must not leak into the build.

---

## Dependencies

### Cross-compiler toolchain (i686-elf)

You need binutils and GCC built for the `i686-elf` target. The OSDev wiki has the canonical guide:
https://wiki.osdev.org/GCC_Cross-Compiler

**Quick path on Ubuntu/Debian** — build from source or use a pre-built package:

```bash
# Option A: build from source (recommended for compatibility)
# Follow https://wiki.osdev.org/GCC_Cross-Compiler
# Set PREFIX=$HOME/opt/cross and add $PREFIX/bin to PATH

# Option B: some distros ship a usable package
sudo apt install gcc-i686-elf binutils-i686-elf
```

After setup, the following must be on your `PATH`:
- `i686-elf-gcc`
- `i686-elf-as`
- `i686-elf-ar`

Verify:
```bash
i686-elf-gcc --version
```

### Other build tools

```bash
sudo apt install make qemu-system-x86 grub-common grub-pc-bin xorriso mtools
```

| Tool | Purpose |
|---|---|
| `make` | Build orchestration |
| `qemu-system-i386` | Run the kernel in an emulator |
| `grub-mkrescue` | Create a bootable ISO |
| `grub-file` | Validate the Multiboot binary after build |
| `xorriso` + `mtools` | Required by `grub-mkrescue` |

---

## Building

### 1. Install headers

This sets up the `sysroot/` staging directory and copies headers from `libc/` and `kernel/` into it.

```bash
./headers.sh
```

### 2. Build everything

Builds `libc` (as `libk.a`, the freestanding kernel C library) then the kernel itself.

```bash
./build.sh
```

On success the kernel binary lands at:

```
sysroot/boot/quilon.kernel
```

The Makefile automatically validates it as a Multiboot-compliant ELF binary via `grub-file --is-x86-multiboot`.

### Rebuild from scratch

```bash
./clean.sh
./build.sh
```

---

## Running

### Option A — Direct kernel load (fastest)

QEMU loads the ELF binary directly without GRUB. Good for rapid iteration.

```bash
./start-kernel.sh
```

Equivalent command:
```bash
qemu-system-i386 -kernel sysroot/boot/quilon.kernel
```

### Option B — Bootable ISO via GRUB2 (more realistic)

Packages the kernel into a GRUB2 ISO and boots it through the full bootloader path.

```bash
./qemu.sh
```

This calls `iso.sh` internally to produce `quilon.iso`, then boots it with QEMU.

To build the ISO separately:
```bash
./iso.sh          # produces quilon.iso
```

---

## Expected output

The kernel initialises the GDT, IDT, and VGA terminal, then prints:

```
Quilon OS v0.0.1

c this 42
0
1
2
...
199
```

---

## User programs (ELF demo)

`user/hello.S` is a freestanding i386 assembly program that runs at ring 3 using
Quilon's `int $0x80` syscall interface. It is compiled to `user/hello.elf` and
placed on the FAT16 disk image as `HELLO.ELF`.

### Building the ELF manually

```bash
cd user && make
```

This produces `user/hello.elf` — a bare ELF32 i386 executable linked at virtual
address `0x400000` with no standard library dependencies.

### Adding it to the disk image

`create_disk.sh` builds the ELF automatically before creating `disk.img`:

```bash
./create_disk.sh
```

`qemu.sh` also rebuilds the disk whenever `user/hello.S` is newer than `disk.img`,
so editing the user program and running `./qemu.sh` is enough.

### Running the ELF inside Quilon

Once the kernel boots to the shell:

```
quilon> exec HELLO.ELF
```

The kernel will:
1. Open `HELLO.ELF` from the FAT16 disk via `elf_load()`.
2. Allocate physical pages with `pmm_alloc_page()` and map them at `0x400000`
   using `paging_map_page_alloc()`.
3. Copy the ELF segment data into the mapped pages.
4. Switch to ring 3 via `usermode_enter()` and jump to the entry point.
5. The program prints a banner using `SYS_WRITE`, then calls `SYS_EXIT`.

### Writing your own user program

Copy `user/hello.S` as a starting point. The syscall convention is:

| Register | Role |
|---|---|
| `eax` | Syscall number |
| `ebx` | First argument |
| `ecx` | Second argument |
| `edx` | Third argument |

| Number | Name | Arguments |
|---|---|---|
| 1 | `SYS_WRITE` | ebx=fd (1=stdout), ecx=buf, edx=len |
| 3 | `SYS_EXIT`  | ebx=exit code |

Link at `0x400000` (or higher) to avoid the kernel's first-4-MiB region.

---

## Project layout

```
Quilon/
├── build.sh              # Top-level build (headers + libc + kernel)
├── clean.sh              # Remove all build artifacts
├── config.sh             # Sets CC, AR, cross-compile env vars
├── create_disk.sh        # Builds user/hello.elf + creates disk.img
├── headers.sh            # Installs headers into sysroot/
├── iso.sh                # Builds quilon.iso with GRUB2
├── qemu.sh               # iso.sh + QEMU boot (rebuilds disk if ELF changed)
├── start-kernel.sh       # QEMU direct kernel boot
├── user/
│   ├── Makefile          # Builds ELF user programs
│   ├── hello.S           # Demo: print banner via syscalls, then exit
│   └── link.ld           # Linker script: loads at 0x400000
├── kernel/
│   ├── Makefile
│   ├── kernel/kernel.c   # Kernel entry point (kernel_main)
│   ├── kernel/elf.c      # ELF32 loader (section 4.12)
│   ├── include/kernel/   # Public kernel headers
│   └── arch/i386/
│       ├── boot.S        # Multiboot header + ISR stubs
│       ├── linker.ld     # Memory layout (kernel at 1 MB)
│       ├── tty.c         # VGA 80×25 text mode driver
│       ├── gdt.c         # Global Descriptor Table
│       └── interrupts.c  # Interrupt Descriptor Table
├── libc/
│   ├── Makefile
│   ├── include/          # stdio.h, stdlib.h, string.h
│   ├── stdio/            # printf, putchar, puts
│   ├── stdlib/           # abort
│   └── string/           # memcpy, memset, strlen, …
└── sysroot/              # Build staging area (generated)
    └── boot/quilon.kernel
```

---

## Troubleshooting

**`i686-elf-gcc: command not found`**
The cross-compiler is not on `PATH`. Add your toolchain bin directory:
```bash
export PATH="$HOME/opt/cross/bin:$PATH"
```

**`grub-file: command not found`**
Install `grub-common`:
```bash
sudo apt install grub-common
```

**`grub-mkrescue` fails with xorriso/mtools errors**
```bash
sudo apt install xorriso mtools
```

**QEMU window appears but shows nothing / crashes immediately**
Use Option A (`./start-kernel.sh`) which bypasses GRUB and loads the ELF directly — easier to debug early boot issues. Add `-nographic -serial stdio` to see serial output if you wire it up later.
