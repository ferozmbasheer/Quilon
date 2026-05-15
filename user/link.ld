/*
 * user/link.ld -- linker script for Quilon user-space programs
 *
 * Load address: 0x400000 (4 MiB)
 *
 * Why 0x400000?
 *   The kernel identity-maps the first 4 MiB (0x000000–0x3FFFFF) for its
 *   own code, data, stack, and heap.  User programs must live outside that
 *   range.  0x400000 is the start of the next 4-MiB PD slot (page-directory
 *   index 1), which paging_map_page_alloc() will populate on first use.
 *
 * Section layout
 *   .text   -- executable code and read-only data (PT_LOAD, r-x)
 *   .data   -- initialised read/write data          (PT_LOAD, rw-)
 *   .bss    -- uninitialised data, zero-filled       (PT_LOAD, rw-)
 *             p_filesz=0, p_memsz>0: the ELF loader
 *             zeroes these pages via memset before copying file data.
 *
 * /DISCARD/
 *   Strip metadata sections that add size without being useful at runtime:
 *   .note.*   -- build-id, ABI tags
 *   .comment  -- compiler version string
 *   .eh_frame -- C++ exception unwinding tables
 */

ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text :
    {
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
    }

    .data :
    {
        *(.data)
        *(.data.*)
    }

    .bss :
    {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    /DISCARD/ :
    {
        *(.note*)
        *(.comment)
        *(.eh_frame*)
        *(.gnu*)
    }
}
