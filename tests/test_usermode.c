/*
 * Quilon OS — User Mode unit tests
 *
 * Tests the pure-C, architecture-independent parts of user-mode support:
 *   - GDT selector encoding (correct RPL bits, correct indices)
 *   - paging helper: paging_make_entry() flag combinations
 *   - paging helper: virtual-address decomposition macros
 *   - TSS selector value
 *   - user-stack size / alignment constant
 *
 * What is NOT tested here (requires x86 hardware / QEMU):
 *   - usermode_initialize() — calls ltr and modifies live page tables
 *   - usermode_enter()      — executes iret to change CPL
 *   - user_task_demo()      — writes to VGA, triggers GPF
 *
 * Build & run:  cd tests && make
 */

#include "framework.h"
#include <kernel/usermode.h>
#include <kernel/paging.h>

/* ── GDT selector values ────────────────────────────────────────────────────
 *
 * GDT layout: [0] null  [1] kcode  [2] kdata  [3] kstack
 *             [4] ucode [5] udata  [6] ustack  [7] TSS
 *
 * Selector = (index * 8) | TI=0 | RPL
 *   USER_CS = 4*8 | 3 = 32 | 3 = 35 = 0x23
 *   USER_DS = 5*8 | 3 = 40 | 3 = 43 = 0x2B
 *   TSS_SEL = 7*8 | 0 = 56     = 0x38
 */
static void test_gdt_selectors(void)
{
    /* USER_CS: GDT index 4, RPL=3 → (4 * 8) | 3 = 0x23 */
    ASSERT_EQ(USER_CS, 0x23u, "USER_CS encodes GDT[4] with RPL=3");

    /* USER_DS: GDT index 5, RPL=3 → (5 * 8) | 3 = 0x2B */
    ASSERT_EQ(USER_DS, 0x2Bu, "USER_DS encodes GDT[5] with RPL=3");

    /* TSS_SEL: GDT index 7, RPL=0 → (7 * 8) | 0 = 0x38 */
    ASSERT_EQ(TSS_SEL, 0x38u, "TSS_SEL encodes GDT[7] with RPL=0");

    /* RPL field lives in bits 1:0 of the selector */
    ASSERT_EQ(USER_CS & 0x3u, 3u, "USER_CS RPL field is 3");
    ASSERT_EQ(USER_DS & 0x3u, 3u, "USER_DS RPL field is 3");
    ASSERT_EQ(TSS_SEL & 0x3u, 0u, "TSS_SEL RPL field is 0");

    /* TI bit (bit 2) must be 0 for GDT (not LDT) descriptors */
    ASSERT_EQ((USER_CS >> 2) & 0x1u, 0u, "USER_CS TI=0 (GDT)");
    ASSERT_EQ((USER_DS >> 2) & 0x1u, 0u, "USER_DS TI=0 (GDT)");
    ASSERT_EQ((TSS_SEL >> 2) & 0x1u, 0u, "TSS_SEL TI=0 (GDT)");

    /* Descriptor index is the selector shifted right by 3 */
    ASSERT_EQ(USER_CS >> 3, 4u, "USER_CS index is 4");
    ASSERT_EQ(USER_DS >> 3, 5u, "USER_DS index is 5");
    ASSERT_EQ(TSS_SEL >> 3, 7u, "TSS_SEL index is 7");
}

/* ── User-stack constants ────────────────────────────────────────────────── */

static void test_user_stack_constants(void)
{
    /* Stack must be at least one page */
    ASSERT(USER_STACK_SIZE >= 4096u, "USER_STACK_SIZE >= 4096 bytes");

    /* Stack size must be a multiple of 4 KiB (page-aligned) */
    ASSERT_EQ(USER_STACK_SIZE % 4096u, 0u, "USER_STACK_SIZE is page-aligned");
}

/* ── paging_make_entry ───────────────────────────────────────────────────── */

static void test_paging_make_entry(void)
{
    /* Physical address is stored in bits 31:12; low 12 bits are flags. */
    uint32_t e = paging_make_entry(0x1000u, PAGE_PRESENT);
    ASSERT_EQ(e & ~0xFFFu, 0x1000u, "physical address preserved in bits 31:12");
    ASSERT_EQ(e & PAGE_PRESENT, PAGE_PRESENT, "PAGE_PRESENT flag set");
    ASSERT_EQ(e & PAGE_WRITABLE, 0u, "PAGE_WRITABLE not set when not requested");

    /* Multiple flags */
    e = paging_make_entry(0x2000u, PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    ASSERT_EQ(e & ~0xFFFu, 0x2000u, "address preserved with multiple flags");
    ASSERT_EQ(e & PAGE_PRESENT,  PAGE_PRESENT,  "PRESENT set");
    ASSERT_EQ(e & PAGE_WRITABLE, PAGE_WRITABLE, "WRITABLE set");
    ASSERT_EQ(e & PAGE_USER,     PAGE_USER,     "USER set");

    /* Unaligned physical address: low 12 bits must be masked out */
    e = paging_make_entry(0x1FFFu, PAGE_PRESENT);
    ASSERT_EQ(e & ~0xFFFu, 0x1000u, "unaligned addr: low 12 bits stripped");
    ASSERT_EQ(e & PAGE_PRESENT, PAGE_PRESENT, "PRESENT still set after masking");

    /* Zero physical address */
    e = paging_make_entry(0x0u, PAGE_PRESENT | PAGE_USER);
    ASSERT_EQ(e & ~0xFFFu, 0u, "zero physical address");
    ASSERT_EQ(e & (PAGE_PRESENT | PAGE_USER),
              PAGE_PRESENT | PAGE_USER, "flags set on zero-addr entry");
}

/* ── Virtual-address decomposition ──────────────────────────────────────── */

static void test_virt_decompose(void)
{
    /* 0x00000000: PD=0, PT=0, offset=0 */
    ASSERT_EQ(VIRT_PD_INDEX(0x00000000u), 0u,  "0x00000000 → PD index 0");
    ASSERT_EQ(VIRT_PT_INDEX(0x00000000u), 0u,  "0x00000000 → PT index 0");
    ASSERT_EQ(VIRT_OFFSET(0x00000000u),   0u,  "0x00000000 → offset 0");

    /* 0x00001000: PD=0, PT=1, offset=0 (second page in first 4MiB) */
    ASSERT_EQ(VIRT_PD_INDEX(0x00001000u), 0u,  "0x00001000 → PD index 0");
    ASSERT_EQ(VIRT_PT_INDEX(0x00001000u), 1u,  "0x00001000 → PT index 1");
    ASSERT_EQ(VIRT_OFFSET(0x00001000u),   0u,  "0x00001000 → offset 0");

    /* 0x00400000: PD=1, PT=0 (start of second 4-MiB window) */
    ASSERT_EQ(VIRT_PD_INDEX(0x00400000u), 1u,  "0x00400000 → PD index 1");
    ASSERT_EQ(VIRT_PT_INDEX(0x00400000u), 0u,  "0x00400000 → PT index 0");

    /* 0xFFC00000: PD=1023, PT=0 (last PD slot) */
    ASSERT_EQ(VIRT_PD_INDEX(0xFFC00000u), 1023u, "0xFFC00000 → PD index 1023");
    ASSERT_EQ(VIRT_PT_INDEX(0xFFC00000u), 0u,    "0xFFC00000 → PT index 0");

    /* 0xFFFFFFFF: PD=1023, PT=1023, offset=0xFFF */
    ASSERT_EQ(VIRT_PD_INDEX(0xFFFFFFFFu), 1023u, "0xFFFFFFFF → PD index 1023");
    ASSERT_EQ(VIRT_PT_INDEX(0xFFFFFFFFu), 1023u, "0xFFFFFFFF → PT index 1023");
    ASSERT_EQ(VIRT_OFFSET(0xFFFFFFFFu),   0xFFFu,"0xFFFFFFFF → offset 0xFFF");

    /* 0xB8000 (VGA buffer): PD=0, PT=0xB8 (=184), offset=0 */
    ASSERT_EQ(VIRT_PD_INDEX(0x000B8000u), 0u,   "VGA 0xB8000 → PD index 0");
    ASSERT_EQ(VIRT_PT_INDEX(0x000B8000u), 0xB8u,"VGA 0xB8000 → PT index 0xB8");
    ASSERT_EQ(VIRT_OFFSET(0x000B8000u),   0u,   "VGA 0xB8000 → offset 0");
}

/* ── PAGE_USER flag value ────────────────────────────────────────────────── */

static void test_page_flags(void)
{
    /* Per the x86 manual: bit 0=P, bit 1=R/W, bit 2=U/S */
    ASSERT_EQ(PAGE_PRESENT,  1u, "PAGE_PRESENT  = bit 0 = 1");
    ASSERT_EQ(PAGE_WRITABLE, 2u, "PAGE_WRITABLE = bit 1 = 2");
    ASSERT_EQ(PAGE_USER,     4u, "PAGE_USER     = bit 2 = 4");

    /* Flags must not overlap */
    ASSERT_EQ(PAGE_PRESENT & PAGE_WRITABLE, 0u, "PRESENT and WRITABLE don't overlap");
    ASSERT_EQ(PAGE_PRESENT & PAGE_USER,     0u, "PRESENT and USER don't overlap");
    ASSERT_EQ(PAGE_WRITABLE & PAGE_USER,    0u, "WRITABLE and USER don't overlap");
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    RUN_SUITE(test_gdt_selectors);
    RUN_SUITE(test_user_stack_constants);
    RUN_SUITE(test_paging_make_entry);
    RUN_SUITE(test_virt_decompose);
    RUN_SUITE(test_page_flags);
    TEST_SUMMARY();
}
