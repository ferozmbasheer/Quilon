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
#   make disk       — build + write every ELF into $(DISK)      (FAT16)
#   make ext2disk   — build + write every ELF into $(EXT2_DISK) (ext2, needs mke2fs)
#   make clean      — remove all build artefacts (including libc/)
#
# Variables
#   DISK       path to the FAT16 disk image  (default: ../disk.img)
#   EXT2_DISK  path to the ext2 disk image   (default: ../ext2.img)

CC = i686-elf-gcc

LIBC_DIR   = libc
LIBC_A     = $(LIBC_DIR)/libc.a
CRT0_O     = $(LIBC_DIR)/crt0.o

LIBGFX_DIR = libgfx
LIBGFX_A   = $(LIBGFX_DIR)/libgfx.a

# Flags for compiling user programs.
#
# -MMD -MP writes a .d file next to each .o listing the headers it included,
# so editing a libc header (dirent.h, sys/stat.h, …) rebuilds the programs
# that use it.  Without this a struct whose layout the kernel also knows —
# dirent_t is the sharp example — could silently go out of sync with the
# kernel after a header change, producing garbage at runtime from a build
# that looked clean.
CFLAGS  = -m32 -ffreestanding -fno-pie -fno-stack-protector -O2 -Wall \
          -MMD -MP \
          -I$(LIBC_DIR)/include -I$(LIBGFX_DIR)/include

# Flags for the final link step.
# LDLIBS must appear AFTER object files (linker scans left-to-right).
# libgfx before libc because libgfx calls malloc/free from libc.
LDFLAGS = -m32 -ffreestanding -nostdlib -fno-pie -T link.ld
LDLIBS  = -L$(LIBGFX_DIR) -lgfx -L$(LIBC_DIR) -lc -lgcc

# ── Auto-discover C programs ──────────────────────────────────────────────────
# Scan for */main.c, strip the filename, exclude libc/ and wm/
# (wm has a custom link rule below that bundles all graphical apps).
PROG_DIRS := $(filter-out libc wm,$(patsubst %/main.c,%,$(wildcard */main.c)))
# Each dir produces <dir>/<dir>.elf; wm is added manually.
PROG_ELFS := $(foreach d,$(PROG_DIRS),$(d)/$(d).elf) wm/wm.elf

# Assembly demo: lives at user/hello.S, defines its own _start, no libc.
ASM_ELFS = hello.elf

# Disk image path (override: make disk DISK=/path/to/disk.img)
DISK ?= ../disk.img

.PHONY: all clean libc libgfx disk ext2disk

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
define PROG_link
$(1)/$(1).elf: $$(CRT0_O) $(1)/main.o $$(LIBGFX_A) $$(LIBC_A) link.ld
	$$(CC) $$(LDFLAGS) -o $$@ $$(CRT0_O) $(1)/main.o $$(LDLIBS)
endef
$(foreach d,$(PROG_DIRS),$(eval $(call PROG_link,$(d))))

# ── Disk image ────────────────────────────────────────────────────────────────
disk: all
	python3 ../tools/make_disk.py $(DISK) $(ASM_ELFS) $(PROG_ELFS)

# ext2 image with the same contents (section 16.2).  Needs mke2fs on the host.
# The kernel probes ext2 before FAT16, so booting with this image attached
# instead of disk.img exercises the ext2 driver.
EXT2_DISK ?= ../ext2.img

ext2disk: all
	python3 ../tools/make_ext2_disk.py $(EXT2_DISK) $(ASM_ELFS) $(PROG_ELFS)

# ── WM: bundles the compositor + all graphical apps into one ELF ─────────────
# Apps are clone(CLONE_VM) threads; no IPC needed — they share WM heap.

WM_CFLAGS = $(CFLAGS) -Iwm

wm/main.o: wm/main.c wm/wm.h
	$(CC) $(WM_CFLAGS) -c -o $@ wm/main.c

wm/wm_compositor.o: wm/wm_compositor.c wm/wm.h
	$(CC) $(WM_CFLAGS) -c -o $@ wm/wm_compositor.c

gterm/gterm_app.o: gterm/gterm_app.c gterm/gterm.h wm/wm.h
	$(CC) $(WM_CFLAGS) -c -o $@ gterm/gterm_app.c

clock/clock_app.o: clock/clock_app.c wm/wm.h
	$(CC) $(WM_CFLAGS) -c -o $@ clock/clock_app.c

filebr/filebr_app.o: filebr/filebr_app.c wm/wm.h
	$(CC) $(WM_CFLAGS) -c -o $@ filebr/filebr_app.c

wm/power_app.o: wm/power_app.c wm/wm.h
	$(CC) $(WM_CFLAGS) -c -o $@ wm/power_app.c

textview/textview_app.o: textview/textview_app.c wm/wm.h
	$(CC) $(WM_CFLAGS) -c -o $@ textview/textview_app.c

WM_OBJS = wm/main.o wm/wm_compositor.o wm/power_app.o \
          gterm/gterm_app.o \
          clock/clock_app.o \
          filebr/filebr_app.o \
          textview/textview_app.o

wm/wm.elf: $(CRT0_O) $(WM_OBJS) $(LIBGFX_A) $(LIBC_A) link.ld
	$(CC) $(LDFLAGS) -o $@ $(CRT0_O) $(WM_OBJS) $(LDLIBS)

PROG_OBJS := $(foreach d,$(PROG_DIRS),$(d)/main.o)

# ── Header dependencies ───────────────────────────────────────────────────────
# Pull in the .d files written by -MMD (missing ones are ignored on a clean
# tree, hence the leading '-').
DEPS := $(PROG_OBJS:.o=.d) $(WM_OBJS:.o=.d)
-include $(DEPS)

# ── Clean ─────────────────────────────────────────────────────────────────────

clean:
	$(MAKE) -C $(LIBC_DIR) clean
	$(MAKE) -C $(LIBGFX_DIR) clean
	rm -f hello.o $(ASM_ELFS) $(PROG_OBJS) $(PROG_ELFS) $(WM_OBJS) \
	      $(PROG_OBJS:.o=.d) $(WM_OBJS:.o=.d)
