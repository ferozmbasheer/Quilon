/* Link the kernel at 3 GiB + 1 MiB (0xC0100000).
 * GRUB loads each section at its LMA (= VMA - KERNEL_OFFSET = physical 0x100000+).
 * AT() sets the LMA so ELF p_paddr is correct for GRUB to load at 0x100000. */
ENTRY(_start)

KERNEL_OFFSET = 0xC0000000;

SECTIONS
{
    . = 0xC0100000;
    kernel_start = .;

    .text BLOCK(4K) : AT(ADDR(.text) - KERNEL_OFFSET) ALIGN(4K)
    {
        *(.multiboot)
        *(.text)
    }

    .rodata BLOCK(4K) : AT(ADDR(.rodata) - KERNEL_OFFSET) ALIGN(4K)
    {
        *(.rodata)
    }

    .data BLOCK(4K) : AT(ADDR(.data) - KERNEL_OFFSET) ALIGN(4K)
    {
        *(.data)
    }

    .bss BLOCK(4K) : AT(ADDR(.bss) - KERNEL_OFFSET) ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }
    kernel_end = .;
}
