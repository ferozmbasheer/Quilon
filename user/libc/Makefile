# user/libc/Makefile — build the Quilon user-space C library
#
# Produces:
#   libc.a   — archive containing string, stdio, stdlib, and syscall stubs.
#              Linked with every user program via: -L. -lc
#   crt0.o   — C runtime startup object.  Must be the *first* object passed
#              to the linker so that _start is defined before main() is linked.
#
# Requirements:
#   i686-elf-gcc  — Quilon cross-compiler (i686 ELF target, freestanding)
#
# Usage:
#   make          — build libc.a and crt0.o
#   make clean    — remove all build artefacts

CC = i686-elf-gcc
AR = i686-elf-ar

# Flags common to all translation units.
#   -ffreestanding : no host headers or startup files
#   -fno-pie       : no position-independent code (ELF loaded at 0x400000)
#   -fno-stack-protector : no __stack_chk_fail symbol needed
#   -O2            : optimise
#   -MMD -MP       : emit .d header-dependency files (see the -include below)
CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -O2 -Wall \
         -MMD -MP -Iinclude

SRCS   = string.c stdio.c stdlib.c pthread.c signal.c
OBJS   = string.o stdio.o stdlib.o syscall.o pthread.o signal.o

.PHONY: all clean

all: libc.a crt0.o

libc.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

crt0.o: crt0.S
	$(CC) $(CFLAGS) -c -o $@ $<

string.o: string.c
	$(CC) $(CFLAGS) -c -o $@ $<

stdio.o: stdio.c
	$(CC) $(CFLAGS) -c -o $@ $<

stdlib.o: stdlib.c
	$(CC) $(CFLAGS) -c -o $@ $<

syscall.o: syscall.S
	$(CC) $(CFLAGS) -c -o $@ $<

pthread.o: pthread.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Rebuild a .o when any header it includes changes (e.g. include/dirent.h,
# whose layout the kernel also depends on).
-include $(OBJS:.o=.d) crt0.d

clean:
	rm -f *.o *.d libc.a
