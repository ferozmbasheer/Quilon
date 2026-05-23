# Quilon OS — Host-Side Unit Tests
#
# These tests compile OS source files with the native host gcc
# (not the i686-elf cross-compiler) and run them directly on your
# development machine.  No QEMU, no cross-compiler, no ISO.
#
# How it works
# ────────────
# The OS libc/ source files (string, printf) are pure C with no
# architecture-specific code, so they compile fine on x86-64.
# Our versions of strlen, memcpy etc. shadow the host libc's versions
# during linking — that is intentional; we are testing our code.
#
# Usage
# ─────
#   make          build all tests and run them
#   make build    build only, do not run
#   make run      run already-built tests
#   make clean    remove test binaries
#   make help     show this message

CC     = cc
CFLAGS = -Wall -Wextra -g -std=c11 -I../libc/include \
         -fno-builtin
# -fno-builtin: prevents GCC from replacing calls to standard functions
# (printf, memcmp, strlen, etc.) with its own inline equivalents.
# Without this, GCC may call the system libc instead of our implementations,
# defeating the purpose of the tests.  Example: printf("str\n") becomes
# puts("str"), which calls system puts and never touches our mock putchar.

# ── Source files under test ────────────────────────────────────────────────

LIBC_STRING = \
  ../libc/string/memcpy.c  \
  ../libc/string/memmove.c \
  ../libc/string/memset.c  \
  ../libc/string/memcmp.c  \
  ../libc/string/strlen.c  \
  ../libc/string/strcmp.c  \
  ../libc/string/strncmp.c

# printf.c only — putchar.c is deliberately excluded so that the
# mock putchar() defined in test_printf.c is used by the linker.
LIBC_PRINTF = ../libc/stdio/printf.c

# PMM source under test.
PMM_SRC = ../kernel/arch/i386/pmm.c

# Scheduler source under test.
# pit.c is NOT linked because it calls outb/hlt (hardware-only).
# pit_divisor() is an inline function in pit.h and needs no .c file.
SCHEDULER_SRC = ../kernel/kernel/scheduler.c

# Extra include path for kernel headers (<kernel/pmm.h>, <kernel/multiboot.h>,
# <kernel/pit.h>, <kernel/scheduler.h>, ...).
PMM_INCLUDES = -I../kernel/include
KERNEL_INCLUDES = -I../kernel/include

# Include path for user libc headers (needed by test_libc).
USER_LIBC_INCLUDES = -I../user/libc/include

# ── Test binaries ──────────────────────────────────────────────────────────

BINDIR = bin
TESTS  = $(BINDIR)/test_string $(BINDIR)/test_printf $(BINDIR)/test_pmm $(BINDIR)/test_paging $(BINDIR)/test_pit $(BINDIR)/test_usermode $(BINDIR)/test_syscall $(BINDIR)/test_vfs $(BINDIR)/test_fat16 $(BINDIR)/test_elf $(BINDIR)/test_process $(BINDIR)/test_signal $(BINDIR)/test_libc $(BINDIR)/test_pipe $(BINDIR)/test_initrd $(BINDIR)/test_vma $(BINDIR)/test_cow $(BINDIR)/test_pci $(BINDIR)/test_rtl8139 $(BINDIR)/test_net $(BINDIR)/test_vbe $(BINDIR)/test_smp $(BINDIR)/test_ansi $(BINDIR)/test_psf $(BINDIR)/test_waitq $(BINDIR)/test_clone $(BINDIR)/test_mouse $(BINDIR)/test_gfx $(BINDIR)/test_wm $(BINDIR)/test_gterm

.PHONY: all build run clean help

all: run

build: $(BINDIR) $(TESTS)

$(BINDIR):
	mkdir -p $(BINDIR)

# Run all tests; exit with failure if any test binary exits non-zero.
run: build
	@PASS=1; \
	for t in $(TESTS); do \
	  echo ""; \
	  if ! ./$$t; then PASS=0; fi; \
	done; \
	echo ""; \
	if [ $$PASS -eq 1 ]; then \
	  printf "\033[32m\033[1mAll suites passed.\033[0m\n"; \
	else \
	  printf "\033[31m\033[1mOne or more suites FAILED.\033[0m\n"; \
	  exit 1; \
	fi

# ── Per-binary rules ──────────────────────────────────────────────────────

# String library tests
# Compiles our string .c files directly — they shadow host libc equivalents.
$(BINDIR)/test_string: test_string.c framework.h $(LIBC_STRING)
	$(CC) $(CFLAGS) -o $@ test_string.c $(LIBC_STRING)

# printf tests
# Provides a mock putchar() in test_printf.c; putchar.c is NOT linked.
# printf.c calls our mock, which captures output to a buffer for inspection.
$(BINDIR)/test_printf: test_printf.c framework.h $(LIBC_PRINTF) $(LIBC_STRING)
	$(CC) $(CFLAGS) -o $@ test_printf.c $(LIBC_PRINTF) $(LIBC_STRING)

# PMM tests
# Uses pmm_init_range() instead of pmm_initialize() to avoid the
# 32-bit/64-bit pointer-truncation problem on PIE binaries.
$(BINDIR)/test_pmm: test_pmm.c framework.h $(PMM_SRC) $(LIBC_STRING)
	$(CC) $(CFLAGS) $(PMM_INCLUDES) -o $@ test_pmm.c $(PMM_SRC) $(LIBC_STRING)

# Paging tests
# Only exercises paging.h (inline functions + macros) — paging.c is NOT
# linked because it contains x86 inline assembly (CR3/CR0 writes) that
# cannot run on a 64-bit host.  No extra .c files are required.
$(BINDIR)/test_paging: test_paging.c framework.h
	$(CC) $(CFLAGS) $(PMM_INCLUDES) -o $@ test_paging.c

# PIT and scheduler tests
# pit.c is NOT linked (calls outb/hlt).  pit_divisor() is tested via its
# inline definition in pit.h.  scheduler.c contains no x86 asm on the
# host (the ESP swap is guarded by #ifdef __is_kernel), so it links cleanly.
$(BINDIR)/test_pit: test_pit.c framework.h $(SCHEDULER_SRC)
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ test_pit.c $(SCHEDULER_SRC)

# User-mode tests
# Only the pure-C / header-only parts are tested on the host.
# usermode.c itself is NOT linked (it contains x86-only inline asm: ltr,
# iret, invlpg) — the tests exercise only the constants defined in
# usermode.h and the inline helpers from paging.h.
$(BINDIR)/test_usermode: test_usermode.c framework.h
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ test_usermode.c

# Syscall tests
# Links syscall.c directly — the handler contains no x86 asm (the hlt in
# SYS_EXIT is guarded by #ifdef __is_kernel, absent in the host build).
# terminal_write and putchar are provided as mocks inside test_syscall.c.
# putchar.c is excluded so the mock wins at link time.
# interrupts.c is NOT linked (idt_set_gate is inline, but syscall_initialize
# is never called in tests so no reference to IDT[] is emitted).
$(BINDIR)/test_syscall: test_syscall.c framework.h \
                        ../kernel/kernel/syscall.c \
                        $(VMA_SRC) \
                        $(LIBC_PRINTF) $(LIBC_STRING)
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ \
	    test_syscall.c ../kernel/kernel/syscall.c \
	    $(VMA_SRC) \
	    $(LIBC_PRINTF) $(LIBC_STRING)

# VFS tests
# vfs.c is pure C (no asm, no hardware).  Compile it alongside the test.
$(BINDIR)/test_vfs: test_vfs.c framework.h \
                    ../kernel/kernel/vfs.c \
                    $(LIBC_STRING)
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ \
	    test_vfs.c ../kernel/kernel/vfs.c \
	    $(LIBC_STRING)

# FAT16 tests
# fat16.c uses only memcmp/memcpy from string.h (no kernel asm).
# vfs.c is not needed — tests call fat16_vfs_ops functions directly.
$(BINDIR)/test_fat16: test_fat16.c framework.h \
                      ../kernel/kernel/fat16.c \
                      $(LIBC_STRING)
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ \
	    test_fat16.c ../kernel/kernel/fat16.c \
	    $(LIBC_STRING)

# ELF loader tests
# elf.c's elf_validate() and elf_phdr() are pure parsing (no kernel asm, no
# hardware calls).  elf_load() is guarded by #ifdef __is_kernel and is absent
# in the host build, so only the parsing helpers are tested here.
# string.h functions are provided by LIBC_STRING.
$(BINDIR)/test_elf: test_elf.c framework.h \
                    ../kernel/kernel/elf.c \
                    $(LIBC_STRING)
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ \
	    test_elf.c ../kernel/kernel/elf.c \
	    $(LIBC_STRING)

# Process management tests
# process.c is pure C (no asm, no hardware).  The host build skips the
# #ifdef __is_kernel sections so process_launch, gdt_set_kernel_stack, and
# the process_first_run trampoline are all absent.  Only process_init,
# process_create, process_find, and process_pick_next are exercised.
PROCESS_SRC = ../kernel/kernel/process.c
VMA_SRC     = ../kernel/kernel/vma.c

$(BINDIR)/test_process: test_process.c framework.h $(PROCESS_SRC) $(VMA_SRC) $(LIBC_STRING)
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ \
	    test_process.c $(PROCESS_SRC) $(VMA_SRC) $(LIBC_STRING)

# Signal tests
# signal.c's send/bitmask logic is pure C (no asm, no scheduler).
# process.c is needed because signal_send touches process_t.pending_signals.
# The __is_kernel guard around signal_dispatch's scheduler_yield call is
# absent in the host build, so only signal_send and the bitmask behaviour
# are exercised.
SIGNAL_SRC = ../kernel/kernel/signal.c

$(BINDIR)/test_signal: test_signal.c framework.h \
                       $(SIGNAL_SRC) $(PROCESS_SRC) $(VMA_SRC) $(LIBC_STRING)
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ \
	    test_signal.c $(SIGNAL_SRC) $(PROCESS_SRC) $(VMA_SRC) $(LIBC_STRING)

# User libc tests (section 7.1)
# Tests string.c, stdio.c, and stdlib.c from user/libc/ on the host.
# syscall.S (int $0x80 stubs) is NOT linked — mock write() and sbrk() are
# defined inside test_libc.c instead.
# USER_LIBC_INCLUDES ensures <string.h>, <stdio.h>, <stdlib.h> etc. resolve
# to the user libc headers rather than the system ones.
USER_LIBC_STRING = ../user/libc/string.c
USER_LIBC_STDIO  = ../user/libc/stdio.c
USER_LIBC_STDLIB = ../user/libc/stdlib.c

$(BINDIR)/test_libc: test_libc.c framework.h \
                     $(USER_LIBC_STRING) $(USER_LIBC_STDIO) $(USER_LIBC_STDLIB)
	$(CC) $(USER_LIBC_INCLUDES) $(CFLAGS) -o $@ \
	    test_libc.c $(USER_LIBC_STRING) $(USER_LIBC_STDIO) $(USER_LIBC_STDLIB)

# Pipe tests (section 8.2)
# pipe.c is pure C — the scheduler yield paths are guarded by #ifdef __is_kernel
# and absent in the host build.  Only the ring-buffer logic is exercised.
# vfs.c is also linked so vfs_pipe() can be tested (returns -1 in host build).
PIPE_SRC = ../kernel/kernel/pipe.c

$(BINDIR)/test_pipe: test_pipe.c framework.h \
                     $(PIPE_SRC) \
                     ../kernel/kernel/vfs.c \
                     $(LIBC_STRING)
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ \
	    test_pipe.c $(PIPE_SRC) \
	    ../kernel/kernel/vfs.c \
	    $(LIBC_STRING)

# initrd tests (section 8.3)
# initrd.c is pure C (no asm, no hardware).  vfs.c is also linked so the
# VFS integration tests can call vfs_mount / vfs_open / vfs_read etc.
INITRD_SRC = ../kernel/kernel/initrd.c

$(BINDIR)/test_initrd: test_initrd.c framework.h \
                       $(INITRD_SRC) \
                       ../kernel/kernel/vfs.c \
                       $(LIBC_STRING)
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ \
	    test_initrd.c $(INITRD_SRC) \
	    ../kernel/kernel/vfs.c \
	    $(LIBC_STRING)

# VMA tests (section 9.2)
# vma.c is pure C — no x86 asm, no hardware calls.  Compiles on the host.
$(BINDIR)/test_vma: test_vma.c framework.h $(VMA_SRC)
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ test_vma.c $(VMA_SRC)

# CoW fork tests (section 9.3)
# Tests PMM reference counting (pmm.c — pure C) and the CoW page-table
# algorithm (simulated inline in test_cow.c — no paging.c asm needed).
# paging.h is used for PAGE_COW / PAGE_* constants and paging_make_entry().
$(BINDIR)/test_cow: test_cow.c framework.h $(PMM_SRC) $(LIBC_STRING) $(LIBC_PRINTF)
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ \
	    test_cow.c $(PMM_SRC) $(LIBC_STRING) $(LIBC_PRINTF)

# PCI tests (section 10.1)
# pci.c is compiled on the host without __is_kernel, so the #ifdef guards
# exclude all x86 I/O (outl/inl) and hardware-dependent functions.
# Only pci_class_name(), pci_find_device(), pci_get_device(), and the
# static inline pci_config_addr() are exercised here.
PCI_SRC = ../kernel/arch/i386/pci.c

$(BINDIR)/test_pci: test_pci.c framework.h $(PCI_SRC)
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ test_pci.c $(PCI_SRC)

# RTL8139 tests (section 10.2)
# rtl8139.c is compiled without __is_kernel, so all x86 I/O code (#ifdef
# __is_kernel) is excluded.  Only the static-inline helpers from rtl8139.h
# are exercised: rtl8139_bar0_iobase, rtl8139_rx_advance, rtl8139_capr_value,
# rtl8139_rx_aligned_len, and the constant/flag values.
RTL8139_SRC = ../kernel/arch/i386/rtl8139.c

$(BINDIR)/test_rtl8139: test_rtl8139.c framework.h $(RTL8139_SRC)
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ test_rtl8139.c $(RTL8139_SRC)

# TCP/IP stack tests (section 10.3)
# net.h is header-only (pure static-inline helpers) — no .c file is linked.
# The __is_kernel guard keeps all hardware-dependent code out of the host build,
# so the checksum, byte-order, and frame-building functions are fully testable.
$(BINDIR)/test_net: test_net.c framework.h
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ test_net.c

# VBE framebuffer tests (section 10.4)
# vbe.h contains the bitmap font and static-inline helpers (vbe_pixel_offset,
# vbe_glyph_pixel, vbe_term_cols, vbe_term_rows) which are pure C and compile
# on the host.  vbe.c is NOT linked (it calls paging_map_page_alloc and MMIO).
$(BINDIR)/test_vbe: test_vbe.c framework.h
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ test_vbe.c

# SMP tests (section 10.5)
# Tests spinlock.h (pure static-inline), apic.h (pure static-inline helpers),
# and smp.c's mp_parse_config() (pure C, no hardware access).
# apic.c and smp.c kernel-only paths are guarded by #ifdef __is_kernel and
# are absent in the host build.  smp.c is included directly in test_smp.c
# to pull in mp_parse_config() without linking hardware-dependent code.
$(BINDIR)/test_smp: test_smp.c framework.h
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ test_smp.c

# ANSI escape-code parser tests (section 11.1)
# ansi.h is header-only (pure static-inline / static helpers) — no .c file is
# linked.  The parser state machine, param tokeniser, and SGR color table are
# fully exercised on the host without any kernel or hardware dependencies.
$(BINDIR)/test_ansi: test_ansi.c framework.h
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ test_ansi.c

# PSF2 bitmap font loader tests (section 11.2)
# psf.h exposes psf2_parse() and psf2_glyph_pixel() as pure static-inline
# helpers — no .c file is linked.  psf.c is NOT linked because psf2_draw_glyph
# calls vbe_draw_pixel() (hardware-only) and psf2_make_from_builtin references
# the vbe_font8x8 array from vbe.h (kernel init path).  The tests construct
# synthetic PSF2 images in buffers and exercise header validation and pixel access.
$(BINDIR)/test_psf: test_psf.c framework.h
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ test_psf.c

# Wait queue tests (section 12.2)
# waitq.c is pure C (list manipulation + scheduler_yield which is a no-op
# on the host).  process.c and vma.c provide current_process and process_t.
# scheduler.c provides the no-op scheduler_yield for the host build.
WAITQ_SRC = ../kernel/kernel/waitq.c

$(BINDIR)/test_waitq: test_waitq.c framework.h \
                      $(WAITQ_SRC) \
                      $(SCHEDULER_SRC) \
                      $(PROCESS_SRC) \
                      $(VMA_SRC) \
                      $(LIBC_STRING)
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ \
	    test_waitq.c $(WAITQ_SRC) \
	    $(SCHEDULER_SRC) \
	    $(PROCESS_SRC) \
	    $(VMA_SRC) \
	    $(LIBC_STRING)

# Kernel thread / clone tests (section 12.3)
# process.c and vma.c provide the process_t layout and process_init/process_create.
# syscall.h / process.h define SYS_CLONE, CLONE_*, and the thread_group field.
# No x86 asm is exercised — clone handler is guarded by #ifdef __is_kernel.
$(BINDIR)/test_clone: test_clone.c framework.h $(PROCESS_SRC) $(VMA_SRC) $(LIBC_STRING)
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ \
	    test_clone.c $(PROCESS_SRC) $(VMA_SRC) $(LIBC_STRING)

# PS/2 Mouse tests (section 13)
# mouse.h is header-only (pure static-inline helpers) — no .c file is
# linked.  mouse.c contains only #ifdef __is_kernel code (hardware I/O and
# VBE calls) so it is NOT linked on the host.  The helpers mouse_packet_dx,
# mouse_packet_dy, and mouse_clamp are exercised without any hardware access.
$(BINDIR)/test_mouse: test_mouse.c framework.h
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ test_mouse.c

# 2D graphics library tests (section 14.1)
# gfx.c is pure C with no kernel or hardware dependencies.  malloc/free are
# resolved from the host libc; stdlib.h is our user libc header (declarations
# only).  gfx.c does not call syscalls at runtime (gfx_screen_init and
# gfx_flush issue int $0x80, but those functions are never called in tests).
$(BINDIR)/test_gfx: test_gfx.c framework.h \
                    ../user/libgfx/gfx.c \
                    ../user/libgfx/include/gfx.h
	$(CC) $(CFLAGS) -I../user/libgfx/include -I../user/libc/include \
	    -o $@ test_gfx.c ../user/libgfx/gfx.c

# Window Manager & Compositor tests (section 14.2)
# wm.h contains all pure-logic inline functions (no syscalls, no hardware).
# wm_compositor.c provides wm_composite() and wm_draw_cursor() — these call
# libgfx so we link gfx.c alongside.  main.c is NOT linked (contains main()).
$(BINDIR)/test_wm: test_wm.c framework.h \
                   ../user/wm/wm.h \
                   ../user/wm/wm_compositor.c \
                   ../user/libgfx/gfx.c \
                   ../user/libgfx/include/gfx.h
	$(CC) $(CFLAGS) -I../user/libgfx/include -I../user/libc/include \
	    -I../user/wm \
	    -o $@ test_wm.c ../user/wm/wm_compositor.c ../user/libgfx/gfx.c

# Graphical Terminal emulator tests (section 14.3)
# gterm.h is header-only (all static inline) — no .c file is linked.
# The ANSI parser, cell grid, param parser, and scroll logic are fully
# exercised on the host without any kernel, hardware, or libgfx dependency.
$(BINDIR)/test_gterm: test_gterm.c framework.h \
                      ../user/gterm/gterm.h
	$(CC) $(CFLAGS) -I../user/gterm -I../user/libc/include \
	    -o $@ test_gterm.c

# ── Cleanup ────────────────────────────────────────────────────────────────

clean:
	rm -rf $(BINDIR)

help:
	@head -20 Makefile | grep "^#" | sed 's/^# \{0,1\}//'
