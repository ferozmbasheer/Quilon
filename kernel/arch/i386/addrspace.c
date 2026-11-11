/*
 * Quilon OS -- user address-space teardown (REVIEW.md C)
 *
 * Before this existed, exit + wait only flipped a process slot's state, so
 * every exec leaked its page directory, page tables and every user page.
 *
 * Pure C: no asm, no direct memory access to page tables -- the caller's
 * as_ops_t turns a physical address into the table's entries and frees pages.
 * That keeps the walk unit-testable on the host (tests/test_addrspace.c).
 */

#include <stdint.h>
#include <kernel/paging.h>

uint32_t as_destroy(uint32_t pd_phys, const as_ops_t *ops)
{
    if (!pd_phys || !ops) return 0;

    uint32_t *pd    = ops->table(pd_phys, ops->ctx);
    uint32_t  freed = 0;

    /* PDE 0 is the shared identity page table; PDE >= KERNEL_PD_IDX are the
     * shared kernel half.  Only [1, KERNEL_PD_IDX) belongs to this process. */
    for (uint32_t i = 1; i < KERNEL_PD_IDX; i++) {
        uint32_t pde = pd[i];
        if (!(pde & PAGE_PRESENT)) continue;

        uint32_t  pt_phys = pde & ~(uint32_t)0xFFF;
        uint32_t *pt      = ops->table(pt_phys, ops->ctx);

        for (uint32_t j = 0; j < 1024; j++) {
            uint32_t pte = pt[j];
            if (!(pte & PAGE_PRESENT) || (pte & PAGE_NOFREE)) continue;
            ops->free_page(pte & ~(uint32_t)0xFFF, ops->ctx);
            freed++;
        }
        ops->free_page(pt_phys, ops->ctx);
        pd[i] = 0;
    }
    ops->free_page(pd_phys, ops->ctx);
    return freed;
}
