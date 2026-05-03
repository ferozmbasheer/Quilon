/*
 * Quilon OS — VMA unit tests (section 9.2)
 *
 * Tests the Virtual Memory Area API: vma_init, vma_add, vma_find,
 * vma_find_start, vma_extend, vma_remove.
 *
 * All functions in vma.c are pure C with no x86 asm or hardware calls, so
 * they compile and run cleanly on the host with the native gcc.
 *
 * Run:  cc -Wall -std=c11 -I../kernel/include -o bin/test_vma test_vma.c \
 *           ../kernel/kernel/vma.c
 */

#include "framework.h"
#include <kernel/vma.h>

/* ── vma_init ─────────────────────────────────────────────────────────────── */

static void test_init(void)
{
    vma_t vmas[PROC_VMA_MAX];

    /* Fill with garbage first. */
    for (int i = 0; i < PROC_VMA_MAX; i++) {
        vmas[i].start = 0xDEADBEEFu;
        vmas[i].end   = 0xDEADBEEFu;
        vmas[i].flags = 0xFFu;
        vmas[i].used  = 1;
    }

    vma_init(vmas, PROC_VMA_MAX);

    int all_clear = 1;
    for (int i = 0; i < PROC_VMA_MAX; i++) {
        if (vmas[i].used  != 0 ||
            vmas[i].start != 0 ||
            vmas[i].end   != 0 ||
            vmas[i].flags != 0)
            all_clear = 0;
    }
    ASSERT(all_clear, "vma_init: all slots cleared");
}

/* ── vma_add ──────────────────────────────────────────────────────────────── */

static void test_add_basic(void)
{
    vma_t vmas[PROC_VMA_MAX];
    vma_init(vmas, PROC_VMA_MAX);

    int r = vma_add(vmas, PROC_VMA_MAX, 0x401000u, 0x402000u, VMA_R | VMA_X);
    ASSERT_EQ(r, 0, "vma_add: returns 0 on success");
    ASSERT_EQ(vmas[0].start, 0x401000u, "vma_add: start stored");
    ASSERT_EQ(vmas[0].end,   0x402000u, "vma_add: end stored");
    ASSERT_EQ(vmas[0].flags, VMA_R | VMA_X, "vma_add: flags stored");
    ASSERT_EQ(vmas[0].used,  1, "vma_add: slot marked used");
}

static void test_add_multiple(void)
{
    vma_t vmas[PROC_VMA_MAX];
    vma_init(vmas, PROC_VMA_MAX);

    vma_add(vmas, PROC_VMA_MAX, 0x1000u, 0x2000u, VMA_R);
    vma_add(vmas, PROC_VMA_MAX, 0x3000u, 0x5000u, VMA_R | VMA_W);
    vma_add(vmas, PROC_VMA_MAX, 0x8000u, 0xA000u, VMA_R | VMA_W | VMA_ANON);

    ASSERT_EQ(vmas[0].start, 0x1000u, "vma_add multi: slot 0 start");
    ASSERT_EQ(vmas[1].start, 0x3000u, "vma_add multi: slot 1 start");
    ASSERT_EQ(vmas[2].start, 0x8000u, "vma_add multi: slot 2 start");
    ASSERT_EQ(vmas[3].used,  0,       "vma_add multi: slot 3 unused");
}

static void test_add_table_full(void)
{
    vma_t vmas[4];
    vma_init(vmas, 4);

    ASSERT_EQ(vma_add(vmas, 4, 0x1000u, 0x2000u, VMA_R), 0, "add slot 0 ok");
    ASSERT_EQ(vma_add(vmas, 4, 0x2000u, 0x3000u, VMA_R), 0, "add slot 1 ok");
    ASSERT_EQ(vma_add(vmas, 4, 0x3000u, 0x4000u, VMA_R), 0, "add slot 2 ok");
    ASSERT_EQ(vma_add(vmas, 4, 0x4000u, 0x5000u, VMA_R), 0, "add slot 3 ok");
    int full = vma_add(vmas, 4, 0x5000u, 0x6000u, VMA_R);
    ASSERT_EQ(full, -1, "vma_add: -1 when table full");
}

/* ── vma_find ─────────────────────────────────────────────────────────────── */

static void test_find_basic(void)
{
    vma_t vmas[PROC_VMA_MAX];
    vma_init(vmas, PROC_VMA_MAX);
    vma_add(vmas, PROC_VMA_MAX, 0x401000u, 0x402000u, VMA_R | VMA_X);

    /* Address at start of VMA */
    vma_t *v = vma_find(vmas, PROC_VMA_MAX, 0x401000u);
    ASSERT_NOTNULL(v, "vma_find: finds VMA at start address");
    ASSERT_EQ(v->start, 0x401000u, "vma_find: correct start");

    /* Address in the middle */
    v = vma_find(vmas, PROC_VMA_MAX, 0x401FFCu);
    ASSERT_NOTNULL(v, "vma_find: finds VMA in middle");

    /* Address at end (exclusive) — should NOT find */
    v = vma_find(vmas, PROC_VMA_MAX, 0x402000u);
    ASSERT_NULL(v, "vma_find: end is exclusive (not found at 0x402000)");

    /* Address before VMA */
    v = vma_find(vmas, PROC_VMA_MAX, 0x400FFFu);
    ASSERT_NULL(v, "vma_find: before VMA start (not found)");

    /* Address after VMA */
    v = vma_find(vmas, PROC_VMA_MAX, 0x403000u);
    ASSERT_NULL(v, "vma_find: after VMA end (not found)");
}

static void test_find_multiple_vmas(void)
{
    vma_t vmas[PROC_VMA_MAX];
    vma_init(vmas, PROC_VMA_MAX);
    vma_add(vmas, PROC_VMA_MAX, 0x1000u, 0x2000u, VMA_R);
    vma_add(vmas, PROC_VMA_MAX, 0x4000u, 0x8000u, VMA_R | VMA_W);
    vma_add(vmas, PROC_VMA_MAX, 0xB000u, 0xC000u, VMA_R | VMA_STACK);

    vma_t *v;

    v = vma_find(vmas, PROC_VMA_MAX, 0x1500u);
    ASSERT_NOTNULL(v, "vma_find multi: finds VMA 0");
    ASSERT_EQ(v->start, 0x1000u, "vma_find multi: VMA 0 is correct");

    v = vma_find(vmas, PROC_VMA_MAX, 0x6000u);
    ASSERT_NOTNULL(v, "vma_find multi: finds VMA 1");
    ASSERT_EQ(v->start, 0x4000u, "vma_find multi: VMA 1 is correct");

    v = vma_find(vmas, PROC_VMA_MAX, 0xBFFFu);
    ASSERT_NOTNULL(v, "vma_find multi: finds VMA 2");
    ASSERT_EQ(v->flags & VMA_STACK, (uint32_t)VMA_STACK, "vma_find multi: VMA 2 has STACK flag");

    /* Gap between VMA 0 and VMA 1 */
    v = vma_find(vmas, PROC_VMA_MAX, 0x3000u);
    ASSERT_NULL(v, "vma_find multi: gap between VMAs returns NULL");
}

static void test_find_empty_table(void)
{
    vma_t vmas[PROC_VMA_MAX];
    vma_init(vmas, PROC_VMA_MAX);
    vma_t *v = vma_find(vmas, PROC_VMA_MAX, 0x401000u);
    ASSERT_NULL(v, "vma_find: empty table returns NULL");
}

/* ── vma_find_start ───────────────────────────────────────────────────────── */

static void test_find_start(void)
{
    vma_t vmas[PROC_VMA_MAX];
    vma_init(vmas, PROC_VMA_MAX);
    vma_add(vmas, PROC_VMA_MAX, 0x800000u, 0x801000u, VMA_R | VMA_W | VMA_ANON);
    vma_add(vmas, PROC_VMA_MAX, 0x401000u, 0x402000u, VMA_R | VMA_X);

    vma_t *v = vma_find_start(vmas, PROC_VMA_MAX, 0x800000u);
    ASSERT_NOTNULL(v, "vma_find_start: found by start=0x800000");
    ASSERT_EQ(v->end, 0x801000u, "vma_find_start: correct end");

    v = vma_find_start(vmas, PROC_VMA_MAX, 0x800001u);
    ASSERT_NULL(v, "vma_find_start: non-start address returns NULL");

    v = vma_find_start(vmas, PROC_VMA_MAX, 0x401000u);
    ASSERT_NOTNULL(v, "vma_find_start: second VMA found");
    ASSERT_EQ(v->flags & VMA_X, (uint32_t)VMA_X, "vma_find_start: flags correct");
}

/* ── vma_extend ───────────────────────────────────────────────────────────── */

static void test_extend(void)
{
    vma_t vmas[PROC_VMA_MAX];
    vma_init(vmas, PROC_VMA_MAX);
    vma_add(vmas, PROC_VMA_MAX, 0x800000u, 0x801000u, VMA_R | VMA_W | VMA_ANON);

    int r = vma_extend(vmas, PROC_VMA_MAX, 0x800000u, 0x810000u);
    ASSERT_EQ(r, 0, "vma_extend: returns 0 on success");
    ASSERT_EQ(vmas[0].end, 0x810000u, "vma_extend: end updated");
    ASSERT_EQ(vmas[0].start, 0x800000u, "vma_extend: start unchanged");
    ASSERT_EQ(vmas[0].flags, VMA_R | VMA_W | VMA_ANON, "vma_extend: flags unchanged");

    /* After extend, vma_find should cover the newly added range. */
    vma_t *v = vma_find(vmas, PROC_VMA_MAX, 0x80F000u);
    ASSERT_NOTNULL(v, "vma_extend: vma_find covers new range");
}

static void test_extend_not_found(void)
{
    vma_t vmas[PROC_VMA_MAX];
    vma_init(vmas, PROC_VMA_MAX);
    int r = vma_extend(vmas, PROC_VMA_MAX, 0x800000u, 0x900000u);
    ASSERT_EQ(r, -1, "vma_extend: -1 when VMA not found");
}

/* ── vma_remove ───────────────────────────────────────────────────────────── */

static void test_remove(void)
{
    vma_t vmas[PROC_VMA_MAX];
    vma_init(vmas, PROC_VMA_MAX);
    vma_add(vmas, PROC_VMA_MAX, 0x1000u, 0x2000u, VMA_R);
    vma_add(vmas, PROC_VMA_MAX, 0x3000u, 0x4000u, VMA_R | VMA_W);

    vma_remove(vmas, PROC_VMA_MAX, 0x1000u);

    /* Slot 0 should be free. */
    ASSERT_EQ(vmas[0].used, 0, "vma_remove: slot freed");

    /* vma_find should no longer find the removed range. */
    vma_t *v = vma_find(vmas, PROC_VMA_MAX, 0x1500u);
    ASSERT_NULL(v, "vma_remove: vma_find returns NULL after removal");

    /* The other VMA should still be there. */
    v = vma_find(vmas, PROC_VMA_MAX, 0x3500u);
    ASSERT_NOTNULL(v, "vma_remove: other VMA still present");

    /* Removing a non-existent VMA is a no-op. */
    vma_remove(vmas, PROC_VMA_MAX, 0xDEADBEEFu);
    ASSERT(1, "vma_remove: no-op on missing VMA does not crash");
}

static void test_remove_and_reuse(void)
{
    vma_t vmas[2];
    vma_init(vmas, 2);
    vma_add(vmas, 2, 0x1000u, 0x2000u, VMA_R);
    vma_add(vmas, 2, 0x3000u, 0x4000u, VMA_R);

    vma_remove(vmas, 2, 0x1000u);

    /* Slot 0 freed — add a new VMA and confirm it reuses slot 0. */
    int r = vma_add(vmas, 2, 0x5000u, 0x6000u, VMA_W);
    ASSERT_EQ(r, 0, "vma_remove+add: slot reused after removal");

    vma_t *v = vma_find(vmas, 2, 0x5500u);
    ASSERT_NOTNULL(v, "vma_remove+add: new VMA findable");
    ASSERT_EQ(v->flags, (uint32_t)VMA_W, "vma_remove+add: new flags correct");
}

/* ── heap-like sbrk simulation ────────────────────────────────────────────── */

static void test_heap_simulation(void)
{
    /*
     * Simulate the sbrk demand-paging path:
     *
     *   1. First sbrk: no heap VMA yet → vma_add creates it.
     *   2. Subsequent sbrks: vma_extend grows the end.
     *   3. vma_find validates arbitrary addresses within the heap.
     */
    vma_t  vmas[PROC_VMA_MAX];
    vma_init(vmas, PROC_VMA_MAX);

    uint32_t heap_start = 0x800000u;
    uint32_t brk        = heap_start;

    /* sbrk(4096) — first call: create heap VMA. */
    uint32_t new_brk = brk + 4096u;
    if (vma_extend(vmas, PROC_VMA_MAX, heap_start, new_brk) != 0)
        vma_add(vmas, PROC_VMA_MAX, heap_start, new_brk,
                VMA_R | VMA_W | VMA_ANON);
    brk = new_brk;
    ASSERT_EQ(brk, 0x801000u, "heap sim: break after first 4 KiB sbrk");

    vma_t *v = vma_find(vmas, PROC_VMA_MAX, 0x800000u);
    ASSERT_NOTNULL(v, "heap sim: first page in VMA");

    /* sbrk(8192) — extend. */
    new_brk = brk + 8192u;
    if (vma_extend(vmas, PROC_VMA_MAX, heap_start, new_brk) != 0)
        vma_add(vmas, PROC_VMA_MAX, heap_start, new_brk,
                VMA_R | VMA_W | VMA_ANON);
    brk = new_brk;

    v = vma_find(vmas, PROC_VMA_MAX, 0x802FFCu);
    ASSERT_NOTNULL(v, "heap sim: last page of extended heap in VMA");

    /* Address just past the break is NOT in any VMA. */
    v = vma_find(vmas, PROC_VMA_MAX, brk);
    ASSERT_NULL(v, "heap sim: address at break is outside VMA (exclusive end)");
}

/* ── stack VMA ────────────────────────────────────────────────────────────── */

static void test_stack_vma(void)
{
    vma_t vmas[PROC_VMA_MAX];
    vma_init(vmas, PROC_VMA_MAX);

    uint32_t stack_top   = 0xC0000000u;
    uint32_t stack_start = stack_top - 64u * 4096u;   /* 256 KiB */

    vma_add(vmas, PROC_VMA_MAX, stack_start, stack_top,
            VMA_R | VMA_W | VMA_ANON | VMA_STACK);

    /* Top page (initial ESP location) is in the VMA. */
    vma_t *v = vma_find(vmas, PROC_VMA_MAX, stack_top - 4096u);
    ASSERT_NOTNULL(v, "stack VMA: initial stack page covered");
    ASSERT_EQ(v->flags & VMA_STACK, (uint32_t)VMA_STACK, "stack VMA: STACK flag set");

    /* Address well below the initial page but still in VMA. */
    v = vma_find(vmas, PROC_VMA_MAX, stack_start + 4u);
    ASSERT_NOTNULL(v, "stack VMA: bottom of stack region covered");

    /* Address just below the VMA lower bound is NOT in VMA. */
    v = vma_find(vmas, PROC_VMA_MAX, stack_start - 4u);
    ASSERT_NULL(v, "stack VMA: address below VMA start is not found");
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    RUN_SUITE(test_init);
    RUN_SUITE(test_add_basic);
    RUN_SUITE(test_add_multiple);
    RUN_SUITE(test_add_table_full);
    RUN_SUITE(test_find_basic);
    RUN_SUITE(test_find_multiple_vmas);
    RUN_SUITE(test_find_empty_table);
    RUN_SUITE(test_find_start);
    RUN_SUITE(test_extend);
    RUN_SUITE(test_extend_not_found);
    RUN_SUITE(test_remove);
    RUN_SUITE(test_remove_and_reuse);
    RUN_SUITE(test_heap_simulation);
    RUN_SUITE(test_stack_vma);
    TEST_SUMMARY();
    return (fw_failed > 0) ? 1 : 0;
}
