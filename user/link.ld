/*
 * user/link.ld — linker script for Quilon user-space programs
 *
 * Load address: 0x400000 (4 MiB)
 *
 * Why 0x400000?
 *   The kernel identity-maps the first 4 MiB (0x000000–0x3FFFFF) for its
 *   own code, data, stack, and heap.  User programs must live outside that
 *   range.  0x400000 is the start of the next 4-MiB PD slot (page-directory
 *   index 1), which paging_map_page_alloc() will populate on first use.
 *
 * Single PT_LOAD segment
 *   Placing both .text (code) and the inline string data in one section
 *   produces a single PT_LOAD program header.  This keeps the ELF small
 *   and the loader simple.
 *
 * /DISCARD/
 *   Strip metadata sections that add size without being useful at runtime:
 *   .note.*   — build-id, ABI tags
 *   .comment  — compiler version string
 *   .eh_frame — C++ exception unwinding tables (we have none, but GCC
 *               still emits an empty one for C files)
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

    /DISCARD/ :
    {
        *(.note*)
        *(.comment)
        *(.eh_frame*)
        *(.gnu*)
    }
}
