# user/Makefile — build Quilon user-space programs
#
# Produces bare ELF32 i386 executables loaded from the FAT16 disk and
# run via the ring-3 shell command:  quilon> exec SHELL.ELF
#
# Build targets
# ─────────────
#   hello.elf         — original assembly demo (no libc)
#   shell/shell.elf   — ring-3 shell (section 7.2); launched by kernel_main
#   hello_c/helloc.elf — C demo program (section 7.3)
#
# Requirements:
#   i686-elf-gcc   — Quilon cross-compiler toolchain
#
# Usage:
#   make           — build libc, then all ELF binaries
#   make clean     — remove all build artefacts (including libc/)

CC = i686-elf-gcc

LIBC_DIR = libc
LIBC_A   = $(LIBC_DIR)/libc.a
CRT0_O   = $(LIBC_DIR)/crt0.o

# Flags for compiling user programs.
#   -ffreestanding  : no host startup or headers
#   -fno-pie        : no position-independent code
#   -fno-stack-protector : no __stack_chk_fail dependency
CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -O2 -Wall \
         -I$(LIBC_DIR)/include

# Flags for the final link step.
#   LDFLAGS  — options that must appear before the object files
#   LDLIBS   — libraries that must appear AFTER object files
#              (the linker scans left-to-right; -lc/-lgcc must follow the
#               objects that reference them so undefined symbols are resolved)
LDFLAGS = -m32 -ffreestanding -nostdlib -fno-pie -T link.ld
LDLIBS  = -L$(LIBC_DIR) -lc -lgcc

ELFS = hello.elf shell/shell.elf hello_c/helloc.elf

.PHONY: all clean libc

all: libc $(ELFS)

libc:
	$(MAKE) -C $(LIBC_DIR)

# ── Assembly hello demo (defines its own _start; no libc needed) ───────

hello.o: hello.S
	$(CC) -m32 -ffreestanding -fno-pie -c -o $@ $<

hello.elf: hello.o link.ld
	$(CC) -m32 -ffreestanding -nostdlib -fno-pie \
	    -T link.ld -o $@ hello.o

# ── Ring-3 shell (section 7.2) ─────────────────────────────────────────

shell/main.o: shell/main.c
	$(CC) $(CFLAGS) -c -o $@ $<

shell/shell.elf: $(CRT0_O) shell/main.o $(LIBC_A) link.ld
	$(CC) $(LDFLAGS) -o $@ $(CRT0_O) shell/main.o $(LDLIBS)

# ── Hello C demo (section 7.3) ─────────────────────────────────────────

hello_c/main.o: hello_c/main.c
	$(CC) $(CFLAGS) -c -o $@ $<

hello_c/helloc.elf: $(CRT0_O) hello_c/main.o $(LIBC_A) link.ld
	$(CC) $(LDFLAGS) -o $@ $(CRT0_O) hello_c/main.o $(LDLIBS)

# ── Clean ──────────────────────────────────────────────────────────────

clean:
	$(MAKE) -C $(LIBC_DIR) clean
	rm -f *.o $(ELFS) shell/main.o hello_c/main.o
