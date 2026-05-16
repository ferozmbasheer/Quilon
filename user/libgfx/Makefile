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

CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -O2 -Wall \
         -Iinclude -I../libc/include

.PHONY: all clean

all: libgfx.a

libgfx.a: gfx.o
	$(AR) rcs $@ gfx.o

gfx.o: gfx.c include/gfx.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o libgfx.a
