/*
 * Quilon OS -- PMM Unit Tests
 *
 * Tests the bitmap allocator logic in kernel/arch/i386/pmm.c.
 * Compiled with the host gcc -- no cross-compiler or QEMU needed.
 *
 * Uses pmm_init_range() instead of pmm_initialize() to avoid the
 * 32-bit/64-bit pointer-truncation problem: pmm_initialize() stores
 * the multiboot mmap pointer as uint32_t and casts it back inside
 * the kernel; on a 64-bit PIE binary that round-trip is broken.
 * pmm_init_range() takes plain uint32_t physical addresses and
 * exercises exactly the same bitmap logic.
 *
 * Build & run:  make (from tests/)
 */

#include "framework.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>   /* our libc memset -- used by pmm_init_range */

#include <kernel/pmm.h>

/* Stub for the linker symbols referenced by pmm.c's pmm_initialize().
 * Not used by pmm_init_range(), but the linker needs them present.   */
uint32_t kernel_start;
uint32_t kernel_end;

/* -- Test region ------------------------------------------------------------
 * Simulate 15 MiB of available RAM starting at 1 MiB.
 * "Kernel" occupies the first 16 pages (64 KiB) of that region.        */
#define MMAP_BASE    0x100000u          /* 1 MiB  */
#define MMAP_LEN     0xF00000u          /* 15 MiB */
#define MMAP_PAGES   (MMAP_LEN / PAGE_SIZE)   /* 3840 */

#define KERN_BASE    MMAP_BASE
#define KERN_LEN     (16 * PAGE_SIZE)   /* 16 pages = 64 KiB */
#define KERN_PAGES   16

/* After init: null page(1) + kernel pages(16) are reserved.
 * Expected free = 3840 - 16 = 3824.                                    */
#define EXPECTED_FREE  (MMAP_PAGES - KERN_PAGES)

static void setup(void)
{
    pmm_init_range(MMAP_BASE, MMAP_LEN, KERN_BASE, KERN_LEN);
}

/* ===============================================================
 * Initialisation
 * =============================================================== */

static void test_init_free_count(void)
{
    setup();
    ASSERT_EQ(pmm_free_page_count(), (uint32_t)EXPECTED_FREE,
              "free count matches available pages minus reserved");
}

static void test_null_page_reserved(void)
{
    setup();
    /* First-fit returns the lowest available page.
     * If page 0 were free it would be returned first.               */
    void *p = pmm_alloc_page();
    ASSERT_NOTNULL(p, "first alloc succeeds");
    ASSERT((uintptr_t)p != 0, "page 0 (null page) is never allocated");
}

static void test_kernel_pages_reserved(void)
{
    setup();
    /* Kernel occupies KERN_BASE .. KERN_BASE+KERN_LEN.
     * The first allocation should be the page right after the kernel. */
    void *p = pmm_alloc_page();
    ASSERT_NOTNULL(p, "first alloc succeeds");
    ASSERT((uintptr_t)p >= KERN_BASE + KERN_LEN,
           "first allocation is past the reserved kernel region");
}

/* ===============================================================
 * Allocation properties
 * =============================================================== */

static void test_alloc_page_aligned(void)
{
    setup();
    for (int i = 0; i < 16; i++) {
        void *p = pmm_alloc_page();
        ASSERT_NOTNULL(p, "alloc succeeds");
        ASSERT(((uintptr_t)p % PAGE_SIZE) == 0, "returned address is 4 KiB aligned");
    }
}

static void test_alloc_unique(void)
{
    setup();
    void *a = pmm_alloc_page();
    void *b = pmm_alloc_page();
    ASSERT_NOTNULL(a, "first alloc non-NULL");
    ASSERT_NOTNULL(b, "second alloc non-NULL");
    ASSERT_NE((uintptr_t)a, (uintptr_t)b, "two allocations return distinct pages");
}

static void test_alloc_decrements_count(void)
{
    setup();
    uint32_t before = pmm_free_page_count();
    pmm_alloc_page();
    ASSERT_EQ(pmm_free_page_count(), before - 1, "alloc decrements free count by 1");
}

/* ===============================================================
 * Free
 * =============================================================== */

static void test_free_increments_count(void)
{
    setup();
    void *p = pmm_alloc_page();
    uint32_t after_alloc = pmm_free_page_count();
    pmm_free_page(p);
    ASSERT_EQ(pmm_free_page_count(), after_alloc + 1, "free increments count by 1");
}

static void test_free_then_realloc(void)
{
    setup();
    void *p = pmm_alloc_page();
    pmm_free_page(p);
    void *q = pmm_alloc_page();
    /* First-fit: the just-freed page must be re-used immediately. */
    ASSERT_EQ((uintptr_t)p, (uintptr_t)q, "freed page is returned by next alloc");
}

static void test_double_free_idempotent(void)
{
    setup();
    void *p = pmm_alloc_page();
    pmm_free_page(p);
    uint32_t count = pmm_free_page_count();
    pmm_free_page(p);   /* second free of the same page */
    ASSERT_EQ(pmm_free_page_count(), count, "double free does not corrupt the count");
}

/* ===============================================================
 * Out-of-memory + exhaustion
 * =============================================================== */

static void test_oom_returns_null(void)
{
    setup();
    while (pmm_free_page_count() > 0)
        pmm_alloc_page();

    ASSERT_EQ(pmm_free_page_count(), 0u, "free count is 0 when exhausted");
    ASSERT_NULL(pmm_alloc_page(), "returns NULL when out of memory");
}

static void test_alloc_free_cycle(void)
{
    setup();
    void *pages[64];

    for (int i = 0; i < 64; i++) {
        pages[i] = pmm_alloc_page();
        ASSERT_NOTNULL(pages[i], "alloc succeeds during fill");
    }

    uint32_t after_alloc = pmm_free_page_count();

    for (int i = 0; i < 64; i++)
        pmm_free_page(pages[i]);

    ASSERT_EQ(pmm_free_page_count(), after_alloc + 64,
              "free count recovers after freeing 64 pages");
}

/* ===============================================================
 * main
 * =============================================================== */

int main(void)
{
    RUN_SUITE(test_init_free_count);
    RUN_SUITE(test_null_page_reserved);
    RUN_SUITE(test_kernel_pages_reserved);
    RUN_SUITE(test_alloc_page_aligned);
    RUN_SUITE(test_alloc_unique);
    RUN_SUITE(test_alloc_decrements_count);
    RUN_SUITE(test_free_increments_count);
    RUN_SUITE(test_free_then_realloc);
    RUN_SUITE(test_double_free_idempotent);
    RUN_SUITE(test_oom_returns_null);
    RUN_SUITE(test_alloc_free_cycle);
    TEST_SUMMARY();
}
