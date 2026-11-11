# user/libgfx/Makefile -- build the Quilon 2D graphics library (section 14.1)
#
# Produces:
#   libgfx.a  -- archive containing canvas management and drawing primitives.
#                Linked with compositor, gterm, clock, and other GUI programs.
#
# Requirements:
#   i686-elf-gcc  -- Quilon cross-compiler

CC = i686-elf-gcc
AR = i686-elf-ar

# -MMD -MP emits .d header-dependency files (see the -include below).
CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -O2 -Wall \
         -MMD -MP -Iinclude -I../libc/include

.PHONY: all clean

all: libgfx.a

libgfx.a: gfx.o
	$(AR) rcs $@ gfx.o

gfx.o: gfx.c include/gfx.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Rebuild gfx.o when any header it includes changes.
-include gfx.d

clean:
	rm -f *.o *.d libgfx.a
