# user/Makefile — build Quilon user-space programs
#
# Produces bare ELF32 i386 executables with no standard library.
# These are placed on the FAT16 disk image by create_disk.sh and run
# with the shell command:  quilon> exec HELLO.ELF
#
# Requirements:
#   i686-elf-gcc   — part of the Quilon cross-compiler toolchain
#
# Usage:
#   make           — build all ELF binaries
#   make clean     — remove object files and ELF binaries

CC  = i686-elf-gcc
LD  = i686-elf-ld

# -m32            : generate 32-bit code
# -ffreestanding  : no host headers or startup files
# -fno-pie        : no position-independent code (absolute addressing)
# -fno-stack-protector : no __stack_chk_fail external reference
# -O2             : optimise
CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -O2 -Wall

LDFLAGS = -m elf_i386 -T link.ld --strip-all

ELFS = hello.elf

.PHONY: all clean

all: $(ELFS)

hello.o: hello.S
	$(CC) $(CFLAGS) -c -o $@ $<

hello.elf: hello.o link.ld
	$(LD) $(LDFLAGS) -o $@ hello.o

clean:
	rm -f *.o $(ELFS)
