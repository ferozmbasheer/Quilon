CC = i686-elf-gcc
CFLAGS = -ffreestanding -O2
AS = i686-elf-as

default: quilon.bin

quilon.bin:  linker.ld boot.o kernel.o 
	$(CC) $(CFLAGS) -T linker.ld -o target/quilon.bin target/boot.o target/kernel.o -nostdlib -lgcc

boot.o:  boot.s
	$(AS) boot.s -o target/boot.o

kernel.o:  kernel.c 
	$(CC) $(CFLAGS) -c kernel.c -o target/kernel.o -std=gnu99 -Wall -Wextra

clean: 
	$(RM) target/*