#include <stdint.h>

#include <kernel/paging.h>

/* Page directory and first page table: statically allocated and 4 KiB-
 * aligned so the CPU will accept them as PD/PT base addresses.
 * Living in .bss they are zero-initialised before kernel_main runs, so
 * every unset entry is already marked not-present (bit 0 = 0).          */
static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t first_page_table[1024] __attribute__((aligned(4096)));

void paging_initialize(void)
{
    /* Identity-map the first 4 MiB (1024 pages × 4 KiB = 0x00000000 –
     * 0x003FFFFF).  This keeps every address the kernel is currently
     * using valid after the MMU is switched on, because virtual address
     * == physical address for this range.                               */
    for (int i = 0; i < 1024; i++)
        first_page_table[i] = paging_make_entry(
            (uint32_t)i * PAGE_SIZE, PAGE_PRESENT | PAGE_WRITABLE);

    /* Install the page table into PD slot 0.
     * Slot 0 covers virtual 0x00000000 – 0x003FFFFF.                   */
    page_directory[0] = paging_make_entry(
        (uint32_t)first_page_table, PAGE_PRESENT | PAGE_WRITABLE);

    /* Remaining 1023 PD slots remain 0 (not-present) — any access to
     * virtual addresses >= 4 MiB will raise a page fault (vector 14),
     * which the exception handler will catch and report.                */

    /* 1. Point CR3 at the page directory (physical address).
     * 2. Set bit 31 (PG) in CR0 to turn the MMU on.
     *    "eax" is clobbered by the read-modify-write of CR0.            */
    asm volatile(
        "mov %0,          %%cr3\n\t"
        "mov %%cr0,       %%eax\n\t"
        "or  $0x80000000, %%eax\n\t"
        "mov %%eax,       %%cr0\n\t"
        :
        : "r"(page_directory)
        : "eax"
    );
}
