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
TESTS  = $(BINDIR)/test_string $(BINDIR)/test_printf $(BINDIR)/test_pmm $(BINDIR)/test_paging $(BINDIR)/test_pit $(BINDIR)/test_usermode $(BINDIR)/test_syscall $(BINDIR)/test_vfs $(BINDIR)/test_fat16 $(BINDIR)/test_elf $(BINDIR)/test_process $(BINDIR)/test_signal $(BINDIR)/test_libc

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
                        $(LIBC_PRINTF) $(LIBC_STRING)
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ \
	    test_syscall.c ../kernel/kernel/syscall.c \
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

$(BINDIR)/test_process: test_process.c framework.h $(PROCESS_SRC) $(LIBC_STRING)
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ \
	    test_process.c $(PROCESS_SRC) $(LIBC_STRING)

# Signal tests
# signal.c's send/bitmask logic is pure C (no asm, no scheduler).
# process.c is needed because signal_send touches process_t.pending_signals.
# The __is_kernel guard around signal_dispatch's scheduler_yield call is
# absent in the host build, so only signal_send and the bitmask behaviour
# are exercised.
SIGNAL_SRC = ../kernel/kernel/signal.c

$(BINDIR)/test_signal: test_signal.c framework.h \
                       $(SIGNAL_SRC) $(PROCESS_SRC) $(LIBC_STRING)
	$(CC) $(CFLAGS) $(KERNEL_INCLUDES) -o $@ \
	    test_signal.c $(SIGNAL_SRC) $(PROCESS_SRC) $(LIBC_STRING)

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

# ── Cleanup ────────────────────────────────────────────────────────────────

clean:
	rm -rf $(BINDIR)

help:
	@head -20 Makefile | grep "^#" | sed 's/^# \{0,1\}//'
