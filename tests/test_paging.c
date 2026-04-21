/*
 * Quilon OS — Paging Unit Tests
 *
 * Tests the pure logic declared in kernel/include/kernel/paging.h:
 *   - PAGE_* flag constant values
 *   - Virtual address decomposition macros
 *   - paging_make_entry() address masking and flag encoding
 *
 * These tests compile on the host (native gcc, no cross-compiler, no QEMU)
 * because they only exercise header-defined inline functions and macros.
 * The x86 assembly in paging.c (CR3/CR0 writes) is NOT included here.
 *
 * Build & run:  make (from tests/)
 */

#include "framework.h"

#include <stdint.h>
#include <kernel/paging.h>

/* ══════════════════════════════════════════════════════════════════════
 * PAGE_SIZE and flag constants
 * ══════════════════════════════════════════════════════════════════════ */

static void test_page_size(void)
{
    ASSERT_EQ(PAGE_SIZE, 4096u, "PAGE_SIZE == 4096");
    ASSERT_EQ(PAGE_SIZE & (PAGE_SIZE - 1), 0u, "PAGE_SIZE is a power of two");
}

static void test_flag_values(void)
{
    ASSERT_EQ(PAGE_PRESENT,  1u, "PAGE_PRESENT  == bit 0 (value 1)");
    ASSERT_EQ(PAGE_WRITABLE, 2u, "PAGE_WRITABLE == bit 1 (value 2)");
    ASSERT_EQ(PAGE_USER,     4u, "PAGE_USER     == bit 2 (value 4)");
}

static void test_flags_distinct(void)
{
    ASSERT((PAGE_PRESENT & PAGE_WRITABLE) == 0, "PRESENT and WRITABLE do not overlap");
    ASSERT((PAGE_PRESENT & PAGE_USER)     == 0, "PRESENT and USER do not overlap");
    ASSERT((PAGE_WRITABLE & PAGE_USER)    == 0, "WRITABLE and USER do not overlap");
}

/* ══════════════════════════════════════════════════════════════════════
 * Virtual address decomposition
 *
 * x86 two-level paging splits a 32-bit virtual address as:
 *   bits 31..22  → page directory index  (10 bits, selects 4-MiB region)
 *   bits 21..12  → page table index      (10 bits, selects 4-KiB page)
 *   bits 11..0   → byte offset           (12 bits)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_decompose_zero(void)
{
    uint32_t v = 0x00000000u;
    ASSERT_EQ(VIRT_PD_INDEX(v), 0u, "PD index of 0x00000000 == 0");
    ASSERT_EQ(VIRT_PT_INDEX(v), 0u, "PT index of 0x00000000 == 0");
    ASSERT_EQ(VIRT_OFFSET(v),   0u, "offset  of 0x00000000 == 0");
}

static void test_decompose_1mib(void)
{
    /* 1 MiB = 0x00100000
     *   PD index = 0x00100000 >> 22                  = 0
     *   PT index = (0x00100000 >> 12) & 0x3FF = 0x100 = 256
     *   Offset   = 0x00100000 & 0xFFF               = 0           */
    uint32_t v = 0x00100000u;
    ASSERT_EQ(VIRT_PD_INDEX(v), 0u,   "PD index of 1 MiB == 0");
    ASSERT_EQ(VIRT_PT_INDEX(v), 256u, "PT index of 1 MiB == 256");
    ASSERT_EQ(VIRT_OFFSET(v),   0u,   "offset  of 1 MiB == 0");
}

static void test_decompose_4mib_boundary(void)
{
    /* 4 MiB = 0x00400000 — first address covered by PD slot 1.
     *   PD index = 0x00400000 >> 22 = 1
     *   PT index = (0x00400000 >> 12) & 0x3FF = 0
     *   Offset   = 0                                               */
    uint32_t v = 0x00400000u;
    ASSERT_EQ(VIRT_PD_INDEX(v), 1u, "PD index at 4 MiB boundary == 1");
    ASSERT_EQ(VIRT_PT_INDEX(v), 0u, "PT index at 4 MiB boundary == 0");
    ASSERT_EQ(VIRT_OFFSET(v),   0u, "offset  at 4 MiB boundary == 0");
}

static void test_decompose_with_offset(void)
{
    /* Kernel starts at 1 MiB = 0x00100000; add a 0xABC byte offset.
     *   Full address: 0x00100ABC
     *   PD index = 0
     *   PT index = (0x00100ABC >> 12) & 0x3FF = 0x100 = 256
     *   Offset   = 0x00100ABC & 0xFFF = 0xABC = 2748             */
    uint32_t v = 0x00100ABCu;
    ASSERT_EQ(VIRT_PD_INDEX(v), 0u,    "PD index of 0x00100ABC == 0");
    ASSERT_EQ(VIRT_PT_INDEX(v), 256u,  "PT index of 0x00100ABC == 256");
    ASSERT_EQ(VIRT_OFFSET(v),   0xABCu,"offset  of 0x00100ABC == 0xABC");
}

static void test_decompose_max(void)
{
    /* 0xFFFFFFFF: all bits set.
     *   PD index = 0x3FF = 1023
     *   PT index = 0x3FF = 1023
     *   Offset   = 0xFFF = 4095                                    */
    uint32_t v = 0xFFFFFFFFu;
    ASSERT_EQ(VIRT_PD_INDEX(v), 1023u, "PD index of 0xFFFFFFFF == 1023");
    ASSERT_EQ(VIRT_PT_INDEX(v), 1023u, "PT index of 0xFFFFFFFF == 1023");
    ASSERT_EQ(VIRT_OFFSET(v),   4095u, "offset  of 0xFFFFFFFF == 4095");
}

static void test_decompose_arbitrary(void)
{
    /* 0xDEADBEEF:
     *   binary: 1101_1110_1010_1101_1011_1110_1110_1111
     *   PD index (bits 31..22): 1101111010 = 0x37A = 890
     *   PT index (bits 21..12): 1011011011 = 0x2DB = 731
     *   Offset   (bits 11..0):  111011101111 = 0xEEF = 3823        */
    uint32_t v = 0xDEADBEEFu;
    ASSERT_EQ(VIRT_PD_INDEX(v), 890u,  "PD index of 0xDEADBEEF == 890");
    ASSERT_EQ(VIRT_PT_INDEX(v), 731u,  "PT index of 0xDEADBEEF == 731");
    ASSERT_EQ(VIRT_OFFSET(v),   3823u, "offset  of 0xDEADBEEF == 3823");
}

static void test_decompose_last_slot(void)
{
    /* Last PD slot starts at 0xFFC00000 (PD index 1023).
     * First page in that slot: PT index 0, offset 0.               */
    uint32_t v = 0xFFC00000u;
    ASSERT_EQ(VIRT_PD_INDEX(v), 1023u, "PD index of 0xFFC00000 == 1023");
    ASSERT_EQ(VIRT_PT_INDEX(v), 0u,    "PT index of 0xFFC00000 == 0");
    ASSERT_EQ(VIRT_OFFSET(v),   0u,    "offset  of 0xFFC00000 == 0");
}

/* ══════════════════════════════════════════════════════════════════════
 * paging_make_entry
 * ══════════════════════════════════════════════════════════════════════ */

static void test_make_entry_present_writable(void)
{
    uint32_t e = paging_make_entry(0x1000u, PAGE_PRESENT | PAGE_WRITABLE);
    ASSERT(e & PAGE_PRESENT,  "PRESENT  flag is set");
    ASSERT(e & PAGE_WRITABLE, "WRITABLE flag is set");
    ASSERT(!(e & PAGE_USER),  "USER     flag is clear");
}

static void test_make_entry_address_preserved(void)
{
    /* The physical address (bits 31..12) must survive the round-trip. */
    uint32_t phys = 0x200000u;
    uint32_t e    = paging_make_entry(phys, PAGE_PRESENT);
    ASSERT_EQ(e & ~(uint32_t)0xFFFu, phys,
              "physical address is preserved in entry (bits 31..12)");
}

static void test_make_entry_unaligned_address_masked(void)
{
    /* Low 12 bits of phys_addr must be silently discarded. */
    uint32_t e = paging_make_entry(0x1FFFu, PAGE_PRESENT);
    ASSERT_EQ(e & ~(uint32_t)0xFFFu, 0x1000u,
              "unaligned address rounded down to page boundary");
    ASSERT(e & PAGE_PRESENT, "PRESENT flag intact after masking");
}

static void test_make_entry_zero_flags(void)
{
    /* flags == 0 marks the entry as not-present (useful to unmap). */
    uint32_t e = paging_make_entry(0x3000u, 0u);
    ASSERT_EQ(e & 0xFFFu, 0u,    "zero flags → no flag bits set");
    ASSERT_EQ(e, 0x3000u,        "address is still stored");
}

static void test_make_entry_all_user_flags(void)
{
    uint32_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    uint32_t e     = paging_make_entry(0x5000u, flags);
    ASSERT(e & PAGE_PRESENT,  "PRESENT  set");
    ASSERT(e & PAGE_WRITABLE, "WRITABLE set");
    ASSERT(e & PAGE_USER,     "USER     set");
    ASSERT_EQ(e & ~(uint32_t)0xFFFu, 0x5000u, "address preserved with all flags");
}

static void test_make_entry_flags_dont_bleed_into_address(void)
{
    /* The flag bits (low 12 bits) must not corrupt bits 31..12. */
    uint32_t e = paging_make_entry(0x10000u, PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    ASSERT_EQ(e & ~(uint32_t)0xFFFu, 0x10000u,
              "flags do not corrupt the address field");
}

static void test_make_entry_large_address(void)
{
    /* Near the top of 32-bit address space. */
    uint32_t phys = 0xFF000000u;
    uint32_t e    = paging_make_entry(phys, PAGE_PRESENT | PAGE_WRITABLE);
    ASSERT_EQ(e & ~(uint32_t)0xFFFu, phys,
              "large physical address preserved correctly");
}

/* ══════════════════════════════════════════════════════════════════════
 * Reconstruct virtual address from its decomposed parts
 * (sanity check that the three fields cover all 32 bits exactly once)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_decompose_reconstruct(void)
{
    uint32_t test_addrs[] = {
        0x00000000u, 0x00001000u, 0x00100000u, 0x00400000u,
        0x00B8000u,  0xC0000000u, 0xFFFFFFFFu, 0xDEADBEEFu,
    };
    int n = (int)(sizeof(test_addrs) / sizeof(test_addrs[0]));

    for (int i = 0; i < n; i++) {
        uint32_t v   = test_addrs[i];
        uint32_t rec = (VIRT_PD_INDEX(v) << 22)
                     | (VIRT_PT_INDEX(v) << 12)
                     |  VIRT_OFFSET(v);
        ASSERT_EQ(rec, v, "decomposed fields reconstruct original address");
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * main
 * ══════════════════════════════════════════════════════════════════════ */

int main(void)
{
    RUN_SUITE(test_page_size);
    RUN_SUITE(test_flag_values);
    RUN_SUITE(test_flags_distinct);

    RUN_SUITE(test_decompose_zero);
    RUN_SUITE(test_decompose_1mib);
    RUN_SUITE(test_decompose_4mib_boundary);
    RUN_SUITE(test_decompose_with_offset);
    RUN_SUITE(test_decompose_max);
    RUN_SUITE(test_decompose_arbitrary);
    RUN_SUITE(test_decompose_last_slot);

    RUN_SUITE(test_make_entry_present_writable);
    RUN_SUITE(test_make_entry_address_preserved);
    RUN_SUITE(test_make_entry_unaligned_address_masked);
    RUN_SUITE(test_make_entry_zero_flags);
    RUN_SUITE(test_make_entry_all_user_flags);
    RUN_SUITE(test_make_entry_flags_dont_bleed_into_address);
    RUN_SUITE(test_make_entry_large_address);

    RUN_SUITE(test_decompose_reconstruct);

    TEST_SUMMARY();
}
