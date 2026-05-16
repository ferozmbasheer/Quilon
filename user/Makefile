# user/Makefile — build all Quilon user-space programs and the disk image.
#
# Auto-discovery
# ──────────────
# Any subdirectory (except libc/) that contains a main.c is treated as a user
# program.  A new program is added by creating user/<name>/main.c — no changes
# to this Makefile are needed.
#
# Naming convention:  user/<dir>/main.c  →  user/<dir>/<dir>.elf
#                     on disk:               <DIR>.ELF  (uppercased)
#
# Targets
# ───────
#   make            — build libc + all user programs
#   make disk       — build + write every ELF into $(DISK)
#   make clean      — remove all build artefacts (including libc/)
#
# Variables
#   DISK   path to the disk image  (default: ../disk.img)

CC = i686-elf-gcc

LIBC_DIR   = libc
LIBC_A     = $(LIBC_DIR)/libc.a
CRT0_O     = $(LIBC_DIR)/crt0.o

LIBGFX_DIR = libgfx
LIBGFX_A   = $(LIBGFX_DIR)/libgfx.a

# Flags for compiling user programs.
CFLAGS  = -m32 -ffreestanding -fno-pie -fno-stack-protector -O2 -Wall \
          -I$(LIBC_DIR)/include -I$(LIBGFX_DIR)/include

# Flags for the final link step.
# LDLIBS must appear AFTER object files (linker scans left-to-right).
# libgfx before libc because libgfx calls malloc/free from libc.
LDFLAGS = -m32 -ffreestanding -nostdlib -fno-pie -T link.ld
LDLIBS  = -L$(LIBGFX_DIR) -lgfx -L$(LIBC_DIR) -lc -lgcc

# ── Auto-discover C programs ──────────────────────────────────────────────────
# Scan for */main.c, strip the filename, exclude libc/.
PROG_DIRS := $(filter-out libc,$(patsubst %/main.c,%,$(wildcard */main.c)))
# Each dir produces <dir>/<dir>.elf
PROG_ELFS := $(foreach d,$(PROG_DIRS),$(d)/$(d).elf)

# Assembly demo: lives at user/hello.S, defines its own _start, no libc.
ASM_ELFS = hello.elf

# Disk image path (override: make disk DISK=/path/to/disk.img)
DISK ?= ../disk.img

.PHONY: all clean libc libgfx disk

all: libc libgfx $(ASM_ELFS) $(PROG_ELFS)

# ── libc ──────────────────────────────────────────────────────────────────────
libc:
	$(MAKE) -C $(LIBC_DIR)

# ── libgfx ────────────────────────────────────────────────────────────────────
libgfx: libc
	$(MAKE) -C $(LIBGFX_DIR)

# ── Assembly hello demo ───────────────────────────────────────────────────────
hello.o: hello.S
	$(CC) -m32 -ffreestanding -fno-pie -c -o $@ $<

hello.elf: hello.o link.ld
	$(CC) -m32 -ffreestanding -nostdlib -fno-pie -T link.ld -o $@ hello.o

# ── Generic compile rule for any discovered main.c ────────────────────────────
%/main.o: %/main.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ── Per-program link rule (generated for each discovered directory) ────────────
# Expands to, e.g.:
#   shell/shell.elf: $(CRT0_O) shell/main.o $(LIBGFX_A) $(LIBC_A) link.ld
#       $(CC) $(LDFLAGS) -o $@ $(CRT0_O) shell/main.o $(LDLIBS)
define PROG_link
$(1)/$(1).elf: $$(CRT0_O) $(1)/main.o $$(LIBGFX_A) $$(LIBC_A) link.ld
	$$(CC) $$(LDFLAGS) -o $$@ $$(CRT0_O) $(1)/main.o $$(LDLIBS)
endef
$(foreach d,$(PROG_DIRS),$(eval $(call PROG_link,$(d))))

# ── Disk image ────────────────────────────────────────────────────────────────
# Writes the assembly demo and every auto-discovered ELF into $(DISK).
# To add a new program to the disk: just create user/<name>/main.c.
disk: all
	python3 ../tools/make_disk.py $(DISK) $(ASM_ELFS) $(PROG_ELFS)

# ── Clean ─────────────────────────────────────────────────────────────────────
PROG_OBJS := $(foreach d,$(PROG_DIRS),$(d)/main.o)

clean:
	$(MAKE) -C $(LIBC_DIR) clean
	$(MAKE) -C $(LIBGFX_DIR) clean
	rm -f hello.o $(ASM_ELFS) $(PROG_OBJS) $(PROG_ELFS)
