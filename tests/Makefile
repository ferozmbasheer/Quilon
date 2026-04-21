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
  ../libc/string/strlen.c

# printf.c only — putchar.c is deliberately excluded so that the
# mock putchar() defined in test_printf.c is used by the linker.
LIBC_PRINTF = ../libc/stdio/printf.c

# PMM source under test.
PMM_SRC = ../kernel/arch/i386/pmm.c

# Extra include path for kernel headers (<kernel/pmm.h>, <kernel/multiboot.h>).
PMM_INCLUDES = -I../kernel/include

# ── Test binaries ──────────────────────────────────────────────────────────

BINDIR = bin
TESTS  = $(BINDIR)/test_string $(BINDIR)/test_printf $(BINDIR)/test_pmm $(BINDIR)/test_paging

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

# ── Cleanup ────────────────────────────────────────────────────────────────

clean:
	rm -rf $(BINDIR)

help:
	@head -20 Makefile | grep "^#" | sed 's/^# \{0,1\}//'
