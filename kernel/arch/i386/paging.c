#include <stdint.h>
#include <string.h>

#include <kernel/paging.h>
#include <kernel/pmm.h>

/* Page directory and first page table: statically allocated and 4 KiB-
 * aligned so the CPU will accept them as PD/PT base addresses.
 * Living in .bss they are zero-initialised before kernel_main runs, so
 * every unset entry is already marked not-present (bit 0 = 0).          */
/* Not static: paging_create_address_space() and paging_get_kernel_pd()
 * need access to copy the kernel entries into new page directories.     */
uint32_t page_directory[1024] __attribute__((aligned(4096)));
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

void paging_set_user_access(uint32_t virt_start, uint32_t virt_end)
{
    /* Walk every page in [virt_start, virt_end) and add PAGE_USER to
     * the PDE and PTE for any page that is already marked present.
     * Silently skips pages that have no mapping yet.                    */
    for (uint32_t addr = virt_start; addr < virt_end; addr += PAGE_SIZE) {
        uint32_t pd_idx = VIRT_PD_INDEX(addr);
        uint32_t pt_idx = VIRT_PT_INDEX(addr);

        if (!(page_directory[pd_idx] & PAGE_PRESENT))
            continue;

        /* Set USER on the page directory entry covering this 4-MiB slot. */
        page_directory[pd_idx] |= PAGE_USER;

        /* Dereference the page table pointer stored in the PDE. */
        uint32_t *pt = (uint32_t *)(page_directory[pd_idx] & ~0xFFFu);
        if (pt[pt_idx] & PAGE_PRESENT)
            pt[pt_idx] |= PAGE_USER;
    }

    /* Flush the entire TLB by reloading CR3. */
    asm volatile(
        "mov %%cr3, %%eax\n\t"
        "mov %%eax, %%cr3\n\t"
        :: : "eax"
    );
}

int paging_map_page(uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t pd_idx = VIRT_PD_INDEX(virt);
    uint32_t pt_idx = VIRT_PT_INDEX(virt);

    if (!(page_directory[pd_idx] & PAGE_PRESENT))
        return -1;   /* no page table for this PD slot */

    uint32_t *pt = (uint32_t *)(page_directory[pd_idx] & ~0xFFFu);
    pt[pt_idx] = paging_make_entry(phys, flags);

    /* Propagate any new flags (e.g. PAGE_USER) up to the PDE too. */
    page_directory[pd_idx] |= (flags & (PAGE_USER | PAGE_WRITABLE));

    /* Invalidate just this one TLB entry. */
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");

    return 0;
}

/* ── Per-process address space helpers (section 5.1) ───────────────────── */

/*
 * paging_get_kernel_pd — return a pointer to the kernel page directory.
 *
 * Used when restoring the kernel's address space after running a process
 * in its own page directory (e.g. after exec_longjmp in shell_cmd_exec).
 */
uint32_t *paging_get_kernel_pd(void)
{
    return page_directory;
}

/*
 * paging_create_address_space — allocate a new page directory for a process.
 *
 * Allocates a fresh 4-KiB-aligned page directory via pmm_alloc_page() and
 * copies the kernel's PD entry 0 (the first 4-MiB identity-mapped page table)
 * into slot 0 of the new directory.
 *
 * By sharing the same physical page table, every process can reach the kernel
 * at the same virtual addresses as the kernel itself.  User pages (e.g. the
 * ELF segments at 0x400000) are mapped in separate slots, so they are private
 * to each process even if two processes load at the same virtual address.
 *
 * Returns the new page directory pointer (physical == virtual because the
 * PMM allocates from the identity-mapped first 4 MiB), or NULL on OOM.
 */
uint32_t *paging_create_address_space(void)
{
    uint32_t *new_pd = (uint32_t *)pmm_alloc_page();
    if (!new_pd) return NULL;
    memset(new_pd, 0, PAGE_SIZE);

    /* Share the kernel's first-4-MiB page table so the kernel remains
     * accessible inside every process's address space.                  */
    new_pd[0] = page_directory[0];

    return new_pd;
}

/*
 * paging_switch — switch the active page directory to pd_phys.
 *
 * Writing CR3 atomically replaces the MMU's view of virtual memory and
 * flushes all TLB entries (except global pages, which we don't use).
 * After this instruction, every memory access uses the new mapping.
 */
void paging_switch(uint32_t pd_phys)
{
    asm volatile("mov %0, %%cr3" :: "r"(pd_phys) : "memory");
}

/*
 * paging_map_page_alloc_into — map a page into an arbitrary page directory.
 *
 * Like paging_map_page_alloc() but operates on pd[] instead of the global
 * page_directory[].  Used by elf_load_into() to populate the page directory
 * of a child process before it is first scheduled.
 *
 * Because all PMM pages are allocated from the identity-mapped first 4 MiB,
 * we can access the target page directory and any page tables it points to
 * using their physical addresses directly (phys addr == virt addr there).
 *
 * No TLB flush is performed: the target page directory is not currently
 * loaded in CR3, so there are no stale TLB entries to invalidate.
 */
int paging_fork_address_space(uint32_t *parent_pd, uint32_t *child_pd)
{
    /* Entry 0 is the shared kernel page table — already in child_pd[0].
     * Walk entries 1-1023 for user pages.                                 */
    for (int i = 1; i < 1024; i++) {
        if (!(parent_pd[i] & PAGE_PRESENT))
            continue;

        /* Obtain a pointer to the parent's page table (identity-mapped). */
        uint32_t *parent_pt = (uint32_t *)(parent_pd[i] & ~(uint32_t)0xFFF);

        /* Allocate a fresh page table for the child. */
        uint32_t *child_pt = (uint32_t *)pmm_alloc_page();
        if (!child_pt) return -1;
        memset(child_pt, 0, PAGE_SIZE);

        /* Install in child PD with the same flags as the parent PDE. */
        child_pd[i] = paging_make_entry(
            (uint32_t)(uintptr_t)child_pt,
            parent_pd[i] & (uint32_t)0xFFF);

        /* Copy each present page from parent's PT into child's PT. */
        for (int j = 0; j < 1024; j++) {
            if (!(parent_pt[j] & PAGE_PRESENT))
                continue;

            uint32_t src_phys = parent_pt[j] & ~(uint32_t)0xFFF;
            void *dst_phys = pmm_alloc_page();
            if (!dst_phys) return -1;

            /* Physical == virtual for identity-mapped first 4 MiB. */
            memcpy(dst_phys, (const void *)(uintptr_t)src_phys, PAGE_SIZE);

            /* Map in child's page table with same flags. */
            child_pt[j] = paging_make_entry(
                (uint32_t)(uintptr_t)dst_phys,
                parent_pt[j] & (uint32_t)0xFFF);
        }
    }
    return 0;
}

int paging_map_page_alloc_into(uint32_t *pd, uint32_t virt,
                                uint32_t phys, uint32_t flags)
{
    uint32_t pd_idx = VIRT_PD_INDEX(virt);
    uint32_t pt_idx = VIRT_PT_INDEX(virt);

    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        void *new_pt = pmm_alloc_page();
        if (!new_pt) return -1;
        memset(new_pt, 0, PAGE_SIZE);
        pd[pd_idx] = paging_make_entry(
            (uint32_t)(uintptr_t)new_pt, PAGE_PRESENT | PAGE_WRITABLE);
    }

    uint32_t *pt = (uint32_t *)(pd[pd_idx] & ~(uint32_t)0xFFF);
    pt[pt_idx] = paging_make_entry(phys, flags);
    pd[pd_idx] |= (flags & (PAGE_USER | PAGE_WRITABLE));

    return 0;
}

int paging_map_page_alloc(uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t pd_idx = VIRT_PD_INDEX(virt);
    uint32_t pt_idx = VIRT_PT_INDEX(virt);

    if (!(page_directory[pd_idx] & PAGE_PRESENT)) {
        /* No page table exists for this 4-MiB slot — allocate one.
         *
         * pmm_alloc_page() returns a 4 KiB-aligned physical page.
         * Because the PMM only allocates pages within the first 4 MiB
         * (the identity-mapped region), the physical address equals the
         * virtual address and we can safely write to it via the kernel's
         * identity mapping.                                               */
        void *new_pt = pmm_alloc_page();
        if (!new_pt) return -1;

        /* Zero-initialise: every PTE starts as "not present". */
        memset(new_pt, 0, PAGE_SIZE);

        page_directory[pd_idx] = paging_make_entry(
            (uint32_t)(uintptr_t)new_pt, PAGE_PRESENT | PAGE_WRITABLE);
    }

    uint32_t *pt = (uint32_t *)(page_directory[pd_idx] & ~(uint32_t)0xFFF);
    pt[pt_idx] = paging_make_entry(phys, flags);

    /* Propagate PAGE_USER / PAGE_WRITABLE to the PDE so the CPU allows
     * ring-3 accesses through the page directory entry too.             */
    page_directory[pd_idx] |= (flags & (PAGE_USER | PAGE_WRITABLE));

    /* Invalidate just this one TLB entry. */
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");

    return 0;
}
