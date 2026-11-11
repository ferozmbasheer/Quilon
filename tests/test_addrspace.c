/*
 * test_addrspace.c -- host unit tests for as_destroy (REVIEW.md C).
 *
 * as_destroy walks an x86 page directory and frees what a user address space
 * owns.  Physical memory is simulated by a small array of 4-KiB "frames": a
 * fake physical address N*0x1000 names frames[N].  The free callback records
 * every page it is asked to release, so each test can check exactly what was
 * (and was not) freed.
 */
#include "framework.h"

#include <stdint.h>
#include <string.h>
#include <kernel/paging.h>

#define NFRAMES 32
static uint32_t frames[NFRAMES][1024];
static uint32_t freed[256];
static int      nfreed;

static uint32_t *fake_table(uint32_t phys, void *ctx)
{
    (void)ctx;
    return frames[phys / 0x1000u];
}
static void fake_free(uint32_t phys, void *ctx)
{
    (void)ctx;
    if (nfreed < 256) freed[nfreed] = phys;
    nfreed++;
}
static const as_ops_t ops = { fake_table, fake_free, 0 };

static void reset(void) { memset(frames, 0, sizeof frames); nfreed = 0; }

static int was_freed(uint32_t phys)
{
    int n = 0;
    for (int i = 0; i < nfreed && i < 256; i++) if (freed[i] == phys) n++;
    return n;
}

#define F(n)   ((uint32_t)(n) * 0x1000u)              /* frame n's phys addr */
#define UPTE   (PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER)

static void test_empty_space_frees_only_pd(void)
{
    reset();
    ASSERT_EQ(as_destroy(F(1), &ops), 0u, "no user pages in an empty space");
    ASSERT_EQ(nfreed, 1, "only the page directory is freed");
    ASSERT_EQ(was_freed(F(1)), 1, "the page directory is freed once");
}

static void test_frees_pages_tables_and_pd(void)
{
    reset();
    uint32_t *pd = frames[1];
    /* PDE 1 (0x00400000): program text + data in PT frame 2 */
    pd[1] = F(2) | UPTE;
    frames[2][0] = F(10) | UPTE;
    frames[2][1] = F(11) | UPTE;
    /* PDE 2 (0x00800000): heap in PT frame 3, with a gap */
    pd[2] = F(3) | UPTE;
    frames[3][0]   = F(12) | UPTE;
    frames[3][700] = F(13) | UPTE;
    frames[3][5]   = F(14);              /* not present: must be ignored */
    /* PDE 767 (stack just below 0xC0000000) in PT frame 4 */
    pd[767] = F(4) | UPTE;
    frames[4][1023] = F(15) | UPTE;

    ASSERT_EQ(as_destroy(F(1), &ops), 5u, "five present user pages released");
    for (int n = 10; n <= 13; n++)
        ASSERT_EQ(was_freed(F(n)), 1, "each user page freed exactly once");
    ASSERT_EQ(was_freed(F(15)), 1, "the stack page is freed");
    ASSERT_EQ(was_freed(F(14)), 0, "a non-present PTE is not freed");
    ASSERT_EQ(was_freed(F(2)) + was_freed(F(3)) + was_freed(F(4)), 3,
              "all three page tables are freed");
    ASSERT_EQ(was_freed(F(1)), 1, "the page directory is freed");
    ASSERT_EQ(nfreed, 5 + 3 + 1, "nothing else is freed");
    ASSERT_EQ(pd[1] | pd[2] | pd[767], 0u, "user PDEs are cleared");
}

static void test_shared_kernel_entries_untouched(void)
{
    reset();
    uint32_t *pd = frames[1];
    pd[0] = F(5) | UPTE;                   /* identity map: shared */
    frames[5][3] = F(20) | UPTE;
    pd[KERNEL_PD_IDX]     = F(6) | PAGE_PRESENT | PAGE_WRITABLE;  /* kernel */
    frames[6][0] = F(21) | PAGE_PRESENT;
    pd[KERNEL_PD_IDX + 1] = F(7) | PAGE_PRESENT | PAGE_WRITABLE;  /* kmap/VBE */
    pd[1023]              = F(8) | PAGE_PRESENT | PAGE_WRITABLE;

    ASSERT_EQ(as_destroy(F(1), &ops), 0u, "kernel/identity pages are not user pages");
    ASSERT_EQ(nfreed, 1, "only the PD is freed");
    ASSERT_EQ(was_freed(F(5)) + was_freed(F(20)), 0, "PDE 0 (identity map) kept");
    ASSERT_EQ(was_freed(F(6)) + was_freed(F(21)) + was_freed(F(7)) +
              was_freed(F(8)), 0, "kernel-half tables and pages kept");
    ASSERT_NE(pd[KERNEL_PD_IDX], 0u, "kernel PDE left in place");
}

static void test_nofree_pages_skipped(void)
{
    reset();
    uint32_t *pd = frames[1];
    /* The WM's SYS_GFX_MAP mapping of the kernel's VBE shadow buffer. */
    pd[0x60000000u >> 22] = F(2) | UPTE;
    frames[2][0] = F(16) | UPTE | PAGE_NOFREE;
    frames[2][1] = F(17) | UPTE | PAGE_NOFREE;
    frames[2][2] = F(18) | UPTE;           /* an ordinary page in the same PT */

    ASSERT_EQ(as_destroy(F(1), &ops), 1u, "only the owned page counts");
    ASSERT_EQ(was_freed(F(16)) + was_freed(F(17)), 0,
              "PAGE_NOFREE (shadow framebuffer) pages are not freed");
    ASSERT_EQ(was_freed(F(18)), 1, "the owned page beside them is freed");
    ASSERT_EQ(was_freed(F(2)), 1, "their page table is still freed");
}

static void test_cow_pages_drop_one_reference(void)
{
    reset();
    uint32_t *pd = frames[1];
    pd[1] = F(2) | UPTE;
    /* After fork: CoW pages are read-only + PAGE_COW, still owned (shared). */
    frames[2][0] = F(19) | PAGE_PRESENT | PAGE_USER | PAGE_COW;
    ASSERT_EQ(as_destroy(F(1), &ops), 1u, "a CoW page is released (one ref)");
    ASSERT_EQ(was_freed(F(19)), 1, "pmm_free_page drops exactly one reference");
}

static void test_null_inputs(void)
{
    reset();
    ASSERT_EQ(as_destroy(0, &ops), 0u, "NULL page directory is a no-op");
    ASSERT_EQ(as_destroy(F(1), 0), 0u, "NULL ops is a no-op");
    ASSERT_EQ(nfreed, 0, "nothing freed");
}

int main(void)
{
    RUN_SUITE(test_empty_space_frees_only_pd);
    RUN_SUITE(test_frees_pages_tables_and_pd);
    RUN_SUITE(test_shared_kernel_entries_untouched);
    RUN_SUITE(test_nofree_pages_skipped);
    RUN_SUITE(test_cow_pages_drop_one_reference);
    RUN_SUITE(test_null_inputs);
    TEST_SUMMARY();
}
