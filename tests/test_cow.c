/*
 * Quilon OS — Copy-on-Write fork unit tests (section 9.3)
 *
 * What is tested (pure C, host-compiled):
 *
 *   Part 1 — PMM reference counting
 *     pmm_alloc_page sets refcount = 1
 *     pmm_ref_page   increments refcount
 *     pmm_free_page  decrements refcount; only releases page when refcount == 0
 *     pmm_page_refcount returns the current count
 *
 *   Part 2 — PAGE_COW flag
 *     PAGE_COW is defined as bit 9 (value 0x200)
 *     PAGE_COW does not overlap with PAGE_PRESENT, PAGE_WRITABLE, PAGE_USER
 *
 *   Part 3 — CoW page table logic (simulated in pure C)
 *     Simulates paging_fork_address_space and paging_cow_handle using plain
 *     uint32_t arrays (no x86 asm).  Tests:
 *       - After CoW fork: parent writable PTEs have WRITABLE cleared, COW set
 *       - After CoW fork: child PTEs are identical to modified parent PTEs
 *       - After CoW fork: physical page refcount == 2
 *       - CoW handle (refcount > 1): allocates a new page, copies data,
 *         decrements old refcount, restores WRITABLE
 *       - CoW handle (refcount == 1): sole owner — restores WRITABLE in place
 *         without allocation
 *
 * What is NOT tested (requires x86 asm / QEMU):
 *   paging_fork_address_space() — modifies live page directories, uses invlpg
 *   paging_cow_handle()         — uses invlpg
 *   Exception handler CoW path  — requires ring-3 execution
 *   TLB flush after fork        — requires CR3 write
 *
 * Build & run:  cd tests && make
 */

#include "framework.h"

#include <stdint.h>
#include <string.h>

#include <kernel/pmm.h>
#include <kernel/paging.h>

/* ── Stubs required by pmm.c ────────────────────────────────────────── */
/* pmm_initialize() references these linker symbols; we provide them as
 * plain variables.  pmm_initialize() is never called in tests — we use
 * pmm_init_range() instead — but the symbols must be present for linking. */
uint32_t kernel_start;
uint32_t kernel_end;

/* printf.c (linked via LIBC_PRINTF) calls putchar.  We discard output. */
void putchar(int c) { (void)c; }

/* ══════════════════════════════════════════════════════════════════════
 * Part 1 — PMM reference counting
 * ══════════════════════════════════════════════════════════════════════ */

static void test_pmm_alloc_sets_refcount_1(void)
{
    pmm_init_range(0x1000, 0x10000, 0, 0);

    void *page = pmm_alloc_page();
    ASSERT_NOTNULL(page, "pmm_alloc_page returns non-NULL");
    ASSERT_EQ(pmm_page_refcount(page), 1u, "freshly allocated page has refcount 1");

    pmm_free_page(page);
}

static void test_pmm_ref_increments(void)
{
    pmm_init_range(0x1000, 0x10000, 0, 0);

    void *page = pmm_alloc_page();
    ASSERT_EQ(pmm_page_refcount(page), 1u, "refcount starts at 1");

    pmm_ref_page(page);
    ASSERT_EQ(pmm_page_refcount(page), 2u, "pmm_ref_page increments to 2");

    pmm_ref_page(page);
    ASSERT_EQ(pmm_page_refcount(page), 3u, "pmm_ref_page increments to 3");

    /* Clean up */
    pmm_free_page(page);
    pmm_free_page(page);
    pmm_free_page(page);
}

static void test_pmm_free_decrements_before_releasing(void)
{
    pmm_init_range(0x1000, 0x10000, 0, 0);

    void    *page       = pmm_alloc_page();
    uint32_t free_after_alloc = pmm_free_page_count();

    pmm_ref_page(page);   /* refcount = 2 */
    ASSERT_EQ(pmm_page_refcount(page), 2u, "after ref_page refcount == 2");

    /* First free: decrement to 1, page NOT released */
    pmm_free_page(page);
    ASSERT_EQ(pmm_page_refcount(page), 1u, "after first free refcount == 1");
    ASSERT_EQ(pmm_free_page_count(), free_after_alloc,
              "page not yet returned to pool after first free");

    /* Second free: decrement to 0, page released */
    pmm_free_page(page);
    ASSERT_EQ(pmm_free_page_count(), free_after_alloc + 1,
              "page returned to pool after second free");
}

static void test_pmm_free_single_alloc_releases_immediately(void)
{
    pmm_init_range(0x1000, 0x10000, 0, 0);

    uint32_t before = pmm_free_page_count();
    void    *page   = pmm_alloc_page();
    ASSERT_EQ(pmm_free_page_count(), before - 1,
              "alloc decrements free count");

    pmm_free_page(page);
    ASSERT_EQ(pmm_free_page_count(), before,
              "single-reference free restores free count");
}

static void test_pmm_multiple_pages_independent_refcounts(void)
{
    pmm_init_range(0x1000, 0x30000, 0, 0);

    void *a = pmm_alloc_page();
    void *b = pmm_alloc_page();
    ASSERT(a != b, "two allocations return different pages");

    pmm_ref_page(a);   /* a: refcount = 2 */
    ASSERT_EQ(pmm_page_refcount(a), 2u, "page a refcount = 2");
    ASSERT_EQ(pmm_page_refcount(b), 1u, "page b refcount = 1 (independent)");

    pmm_free_page(a);
    ASSERT_EQ(pmm_page_refcount(a), 1u, "a refcount decremented to 1");
    ASSERT_EQ(pmm_page_refcount(b), 1u, "b refcount unchanged");

    pmm_free_page(a);
    pmm_free_page(b);
}

static void test_pmm_refcount_after_init_range(void)
{
    pmm_init_range(0x1000, 0x10000, 0, 0);

    /* Before any alloc, all refcounts should be 0 (free pages). */
    /* Allocate and immediately free — refcount for that frame goes
     * 0 → 1 (alloc) → 0 (free).  After free, the frame is back in
     * the pool and its refcount is 0 again.                        */
    void *page = pmm_alloc_page();
    ASSERT_EQ(pmm_page_refcount(page), 1u, "refcount is 1 after alloc");
    pmm_free_page(page);
    ASSERT_EQ(pmm_page_refcount(page), 0u, "refcount is 0 after free");
}

/* ══════════════════════════════════════════════════════════════════════
 * Part 2 — PAGE_COW flag
 * ══════════════════════════════════════════════════════════════════════ */

static void test_page_cow_value(void)
{
    ASSERT_EQ(PAGE_COW, 0x200u, "PAGE_COW == bit 9 (0x200)");
}

static void test_page_cow_no_overlap(void)
{
    ASSERT_EQ(PAGE_COW & PAGE_PRESENT,  0u, "PAGE_COW does not overlap PAGE_PRESENT");
    ASSERT_EQ(PAGE_COW & PAGE_WRITABLE, 0u, "PAGE_COW does not overlap PAGE_WRITABLE");
    ASSERT_EQ(PAGE_COW & PAGE_USER,     0u, "PAGE_COW does not overlap PAGE_USER");
}

static void test_page_cow_distinct_from_pte_flags(void)
{
    /* Standard i386 PTE hw flags live in bits 0-8.  Bit 9 is available. */
    uint32_t hw_flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER
                      | (1u << 3)  /* PWT */
                      | (1u << 4)  /* PCD */
                      | (1u << 5)  /* Accessed */
                      | (1u << 6)  /* Dirty */
                      | (1u << 7)  /* PAT */
                      | (1u << 8); /* Global */
    ASSERT_EQ(PAGE_COW & hw_flags, 0u,
              "PAGE_COW (bit 9) does not alias any hardware PTE flag");
}

static void test_page_cow_paging_make_entry(void)
{
    /* paging_make_entry keeps all 12 flag bits including bit 9. */
    uint32_t phys  = 0x3000u;
    uint32_t flags = PAGE_PRESENT | PAGE_USER | PAGE_COW;
    uint32_t entry = paging_make_entry(phys, flags);
    ASSERT(entry & PAGE_COW,    "PAGE_COW survives paging_make_entry");
    ASSERT(entry & PAGE_PRESENT,"PAGE_PRESENT survives alongside PAGE_COW");
    ASSERT(!(entry & PAGE_WRITABLE), "PAGE_WRITABLE absent when not set");
    ASSERT_EQ(entry & ~(uint32_t)0xFFF, phys,
              "physical address preserved with PAGE_COW set");
}

/* ══════════════════════════════════════════════════════════════════════
 * Part 3 — CoW page-table logic (pure-C simulation)
 *
 * We cannot link paging.c on the host (it has x86 asm).  Instead we
 * replicate the CoW algorithm here using plain uint32_t arrays and the
 * real PMM.  The logic is identical to what the kernel does; only the
 * TLB-flush (invlpg) is omitted.
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * sim_cow_fork_pte — simulate what paging_fork_address_space does to one PTE.
 *
 * If the parent PTE is writable: mark both parent and child CoW, share page.
 * If the parent PTE is read-only: share page unchanged.
 *
 * Increments the physical page's refcount in both cases.
 */
static void sim_cow_fork_pte(uint32_t *parent_pte, uint32_t *child_pte)
{
    uint32_t phys  = *parent_pte & ~(uint32_t)0xFFF;
    uint32_t flags =  *parent_pte & (uint32_t)0xFFF;

    pmm_ref_page((void *)(uintptr_t)phys);

    if (flags & PAGE_WRITABLE) {
        uint32_t cow_flags = (flags & ~(uint32_t)PAGE_WRITABLE) | PAGE_COW;
        *parent_pte = paging_make_entry(phys, cow_flags);
        *child_pte  = paging_make_entry(phys, cow_flags);
    } else {
        *child_pte = paging_make_entry(phys, flags);
    }
}

/*
 * sim_cow_handle — simulate paging_cow_handle for one PTE (no invlpg).
 *
 * Returns 0 on success, -1 if PTE does not have PAGE_COW set or OOM.
 *
 * Note: the memcpy of page content is SKIPPED here because PMM returns
 * physical addresses (e.g. 0x1000) that are not mapped in the host
 * process's virtual address space.  The copy logic is identical to what
 * the kernel does; only the data transfer is omitted in this simulation.
 * Data-copy correctness is verified at runtime in QEMU.
 */
static int sim_cow_handle(uint32_t *pte)
{
    if (!(*pte & PAGE_COW)) return -1;

    uint32_t phys  = *pte & ~(uint32_t)0xFFF;
    uint32_t flags =  *pte & (uint32_t)0xFFF;

    if (pmm_page_refcount((void *)(uintptr_t)phys) <= 1) {
        /* Sole owner: restore writability. */
        uint32_t new_flags = (flags & ~(uint32_t)PAGE_COW) | PAGE_WRITABLE;
        *pte = paging_make_entry(phys, new_flags);
    } else {
        /* Allocate a new page and drop our reference to the original.
         * (memcpy skipped — pages not mapped in host address space.) */
        void *new_phys = pmm_alloc_page();
        if (!new_phys) return -1;
        pmm_free_page((void *)(uintptr_t)phys);  /* drop our reference */
        uint32_t new_flags = (flags & ~(uint32_t)PAGE_COW) | PAGE_WRITABLE;
        *pte = paging_make_entry((uint32_t)(uintptr_t)new_phys, new_flags);
    }
    return 0;
}

static void test_cow_fork_writable_page_marked_cow(void)
{
    /* PMM range big enough for three pages (parent, child, data). */
    pmm_init_range(0x1000, 0x30000, 0, 0);

    /* Simulate a mapped writable user page. */
    void *data_page = pmm_alloc_page();
    uint32_t parent_pte = paging_make_entry(
        (uint32_t)(uintptr_t)data_page,
        PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    uint32_t child_pte  = 0;

    sim_cow_fork_pte(&parent_pte, &child_pte);

    ASSERT(!(parent_pte & PAGE_WRITABLE), "parent PTE: WRITABLE cleared after CoW fork");
    ASSERT(  parent_pte & PAGE_COW,       "parent PTE: COW set after CoW fork");
    ASSERT(!(child_pte  & PAGE_WRITABLE), "child  PTE: WRITABLE cleared after CoW fork");
    ASSERT(  child_pte  & PAGE_COW,       "child  PTE: COW set after CoW fork");

    /* Both PTEs must point to the same physical page. */
    ASSERT_EQ(parent_pte & ~(uint32_t)0xFFF,
              child_pte  & ~(uint32_t)0xFFF,
              "parent and child PTEs share the same physical page");

    pmm_free_page(data_page);
    pmm_free_page(data_page);
}

static void test_cow_fork_readonly_page_not_cow(void)
{
    pmm_init_range(0x1000, 0x30000, 0, 0);

    void *data_page = pmm_alloc_page();
    uint32_t parent_pte = paging_make_entry(
        (uint32_t)(uintptr_t)data_page,
        PAGE_PRESENT | PAGE_USER);  /* read-only */
    uint32_t child_pte = 0;

    sim_cow_fork_pte(&parent_pte, &child_pte);

    ASSERT(!(parent_pte & PAGE_COW), "read-only parent PTE: no PAGE_COW");
    ASSERT(!(child_pte  & PAGE_COW), "read-only child  PTE: no PAGE_COW");
    ASSERT(!(parent_pte & PAGE_WRITABLE), "read-only page stays read-only in parent");
    ASSERT(!(child_pte  & PAGE_WRITABLE), "read-only page stays read-only in child");
    ASSERT_EQ(parent_pte & ~(uint32_t)0xFFF,
              child_pte  & ~(uint32_t)0xFFF,
              "read-only page shared with same physical address");

    pmm_free_page(data_page);
    pmm_free_page(data_page);
}

static void test_cow_fork_increments_refcount(void)
{
    pmm_init_range(0x1000, 0x30000, 0, 0);

    void *data_page = pmm_alloc_page();
    ASSERT_EQ(pmm_page_refcount(data_page), 1u, "refcount starts at 1");

    uint32_t parent_pte = paging_make_entry(
        (uint32_t)(uintptr_t)data_page,
        PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    uint32_t child_pte = 0;

    sim_cow_fork_pte(&parent_pte, &child_pte);

    ASSERT_EQ(pmm_page_refcount(data_page), 2u,
              "refcount == 2 after CoW fork (shared by parent and child)");

    pmm_free_page(data_page);
    pmm_free_page(data_page);
}

static void test_cow_handle_shared_page_copies(void)
{
    /* Shared page: parent + child both reference it → refcount = 2.
     *
     * Note: PMM returns physical addresses (e.g. 0x1000) that are not
     * mapped in the host process's virtual address space.  We only test
     * PTE flag and refcount behavior; memcpy correctness is verified at
     * runtime in QEMU via the 'cow' shell command.                      */
    pmm_init_range(0x1000, 0x50000, 0, 0);

    void *data_page = pmm_alloc_page();

    uint32_t parent_pte = paging_make_entry(
        (uint32_t)(uintptr_t)data_page,
        PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    uint32_t child_pte = 0;

    /* Simulate CoW fork — share the page, mark both CoW. */
    sim_cow_fork_pte(&parent_pte, &child_pte);
    ASSERT_EQ(pmm_page_refcount(data_page), 2u, "refcount == 2 after fork");

    uint32_t old_phys = child_pte & ~(uint32_t)0xFFF;

    /* Child takes a write fault: handle_cow for the child's PTE. */
    uint32_t free_before = pmm_free_page_count();
    int rc = sim_cow_handle(&child_pte);
    ASSERT_EQ(rc, 0, "sim_cow_handle returns 0 (success)");

    /* Child's PTE must now point to a NEW physical page. */
    uint32_t new_phys = child_pte & ~(uint32_t)0xFFF;
    ASSERT(new_phys != old_phys,     "child PTE points to new page after CoW");
    ASSERT(child_pte & PAGE_WRITABLE,"child PTE is now writable");
    ASSERT(!(child_pte & PAGE_COW),  "child PTE: PAGE_COW cleared");

    /* One new page was allocated for the copy. */
    ASSERT_EQ(pmm_free_page_count(), free_before - 1,
              "one additional page allocated for the copy");

    /* Shared page refcount decremented from 2 to 1. */
    ASSERT_EQ(pmm_page_refcount(data_page), 1u,
              "shared page refcount decremented to 1 after CoW handle");

    /* Parent's PTE is unchanged (it still points to original, still CoW). */
    ASSERT_EQ(parent_pte & ~(uint32_t)0xFFF, old_phys,
              "parent PTE still points to original page");
    ASSERT(parent_pte & PAGE_COW, "parent PTE still marked CoW");

    pmm_free_page(data_page);  /* parent's reference (refcount 1 → 0) */
    pmm_free_page((void *)(uintptr_t)new_phys);  /* child's copy */
}

static void test_cow_handle_sole_owner_no_copy(void)
{
    /* If refcount == 1 (sole owner), handle_cow must NOT allocate a new
     * page — it simply restores writability.                             */
    pmm_init_range(0x1000, 0x30000, 0, 0);

    void *data_page = pmm_alloc_page();

    /* Mark as CoW directly (as if the other process already freed its PTE). */
    uint32_t pte = paging_make_entry(
        (uint32_t)(uintptr_t)data_page,
        PAGE_PRESENT | PAGE_COW | PAGE_USER);
    /* refcount is still 1 from alloc. */

    uint32_t free_before = pmm_free_page_count();
    int rc = sim_cow_handle(&pte);
    ASSERT_EQ(rc, 0, "sim_cow_handle returns 0 for sole owner");

    /* PTE should point to the SAME physical page — no copy. */
    ASSERT_EQ(pte & ~(uint32_t)0xFFF, (uint32_t)(uintptr_t)data_page,
              "sole-owner: PTE still points to same physical page");
    ASSERT(pte & PAGE_WRITABLE,  "sole-owner: PAGE_WRITABLE restored");
    ASSERT(!(pte & PAGE_COW),    "sole-owner: PAGE_COW cleared");

    /* No extra allocation. */
    ASSERT_EQ(pmm_free_page_count(), free_before,
              "sole-owner handle: no additional page allocated");

    pmm_free_page(data_page);
}

static void test_cow_handle_non_cow_pte_returns_error(void)
{
    pmm_init_range(0x1000, 0x10000, 0, 0);

    void *data_page = pmm_alloc_page();
    /* Normal writable PTE (no PAGE_COW). */
    uint32_t pte = paging_make_entry(
        (uint32_t)(uintptr_t)data_page,
        PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);

    int rc = sim_cow_handle(&pte);
    ASSERT_EQ(rc, -1, "sim_cow_handle returns -1 for non-CoW PTE");

    /* PTE must be unchanged. */
    ASSERT(pte & PAGE_WRITABLE, "non-CoW PTE unchanged after failed handle");
    ASSERT(!(pte & PAGE_COW),   "non-CoW PTE: PAGE_COW still absent");

    pmm_free_page(data_page);
}

static void test_cow_full_parent_child_write_isolation(void)
{
    /* Full scenario: verify PTE addresses and refcounts through the
     * complete fork + two-write lifecycle.
     *
     *   1. Allocate a shared page (refcount = 1).
     *   2. CoW-fork: both PTEs marked CoW, refcount = 2.
     *   3. Parent takes CoW fault: gets its own page, shared refcount = 1.
     *   4. Child  takes CoW fault: sole owner, in-place (no copy).
     *   5. Parent and child PTEs must point to different physical pages.
     *
     * Actual data content is not compared here (PMM pages are not mapped
     * in host virtual memory); that is verified at runtime in QEMU.     */
    pmm_init_range(0x1000, 0x80000, 0, 0);

    void *shared_page = pmm_alloc_page();

    uint32_t parent_pte = paging_make_entry(
        (uint32_t)(uintptr_t)shared_page,
        PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    uint32_t child_pte = 0;

    sim_cow_fork_pte(&parent_pte, &child_pte);
    ASSERT_EQ(pmm_page_refcount(shared_page), 2u,
              "full scenario: refcount == 2 after fork");

    /* Parent write: refcount > 1 → allocate a copy. */
    int rc = sim_cow_handle(&parent_pte);
    ASSERT_EQ(rc, 0, "full scenario: parent CoW handle succeeds");
    uint32_t parent_phys = parent_pte & ~(uint32_t)0xFFF;
    ASSERT(parent_phys != (uint32_t)(uintptr_t)shared_page,
           "full scenario: parent now has its own page (different phys)");
    ASSERT(parent_pte & PAGE_WRITABLE, "full scenario: parent PTE writable");
    ASSERT(!(parent_pte & PAGE_COW),   "full scenario: parent PAGE_COW cleared");

    /* Shared page refcount drops to 1 (only child still holds it). */
    ASSERT_EQ(pmm_page_refcount(shared_page), 1u,
              "full scenario: shared refcount == 1 after parent copy");

    /* Child write: refcount == 1 (sole owner) → in-place, no copy. */
    rc = sim_cow_handle(&child_pte);
    ASSERT_EQ(rc, 0, "full scenario: child CoW handle succeeds");
    uint32_t child_phys = child_pte & ~(uint32_t)0xFFF;
    ASSERT_EQ(child_phys, (uint32_t)(uintptr_t)shared_page,
              "full scenario: child kept the original page (in-place)");
    ASSERT(child_pte & PAGE_WRITABLE, "full scenario: child PTE writable");
    ASSERT(!(child_pte & PAGE_COW),   "full scenario: child PAGE_COW cleared");

    /* The two processes now own different physical pages. */
    ASSERT(parent_phys != child_phys,
           "full scenario: parent and child pages are distinct");

    pmm_free_page((void *)(uintptr_t)parent_phys);
    pmm_free_page((void *)(uintptr_t)child_phys);   /* was shared_page */
}

/* ══════════════════════════════════════════════════════════════════════
 * main
 * ══════════════════════════════════════════════════════════════════════ */

int main(void)
{
    /* Part 1: PMM reference counting */
    RUN_SUITE(test_pmm_alloc_sets_refcount_1);
    RUN_SUITE(test_pmm_ref_increments);
    RUN_SUITE(test_pmm_free_decrements_before_releasing);
    RUN_SUITE(test_pmm_free_single_alloc_releases_immediately);
    RUN_SUITE(test_pmm_multiple_pages_independent_refcounts);
    RUN_SUITE(test_pmm_refcount_after_init_range);

    /* Part 2: PAGE_COW flag */
    RUN_SUITE(test_page_cow_value);
    RUN_SUITE(test_page_cow_no_overlap);
    RUN_SUITE(test_page_cow_distinct_from_pte_flags);
    RUN_SUITE(test_page_cow_paging_make_entry);

    /* Part 3: CoW page-table logic */
    RUN_SUITE(test_cow_fork_writable_page_marked_cow);
    RUN_SUITE(test_cow_fork_readonly_page_not_cow);
    RUN_SUITE(test_cow_fork_increments_refcount);
    RUN_SUITE(test_cow_handle_shared_page_copies);
    RUN_SUITE(test_cow_handle_sole_owner_no_copy);
    RUN_SUITE(test_cow_handle_non_cow_pte_returns_error);
    RUN_SUITE(test_cow_full_parent_child_write_isolation);

    TEST_SUMMARY();
}
