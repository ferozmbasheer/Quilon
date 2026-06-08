/*
 * Quilon OS -- unit tests for the ATA DMA PRDT helper (section 16.1)
 *
 * Exercises the pure-C static-inline ata_dma_build_prd() in
 * kernel/include/kernel/ata_dma.h.  No kernel code or x86 I/O is touched --
 * the PRD-building rules (size limits, the 64 KiB boundary constraint, and the
 * "64 KiB encodes as byte_count 0" wrap) all run on the host.
 *
 * Build: cc -Wall -Wextra -g -std=c11 -I../kernel/include -o bin/test_ata_dma test_ata_dma.c
 */

#include "framework.h"
#include <kernel/ata_dma.h>

/* -- A normal, well-aligned region ---------------------------------------- */
static void test_basic_region(void)
{
    prdt_entry_t prd;
    /* one sector at a page-aligned address */
    int r = ata_dma_build_prd(&prd, 0x100000, 512);
    ASSERT_EQ(r, 0, "512-byte region at 1 MiB is valid");
    ASSERT_EQ((int)prd.phys_addr, 0x100000, "phys_addr stored verbatim");
    ASSERT_EQ((int)prd.byte_count, 512, "byte_count = 512");
    ASSERT_EQ((int)prd.flags, PRDT_EOT, "single entry marked end-of-table");
}

/* -- 64 KiB transfer encodes as byte_count 0 ------------------------------ */
static void test_full_64k(void)
{
    prdt_entry_t prd;
    /* 64 KiB starting exactly on a 64 KiB boundary stays within it */
    int r = ata_dma_build_prd(&prd, 0x90000, 65536);
    ASSERT_EQ(r, 0, "64 KiB region on a 64 KiB boundary is valid");
    ASSERT_EQ((int)prd.byte_count, 0, "64 KiB encodes as byte_count 0");
    ASSERT_EQ((int)prd.flags, PRDT_EOT, "EOT still set");
}

/* -- Invalid sizes -------------------------------------------------------- */
static void test_bad_sizes(void)
{
    prdt_entry_t prd;
    ASSERT_EQ(ata_dma_build_prd(&prd, 0x100000, 0), -1, "zero bytes rejected");
    ASSERT_EQ(ata_dma_build_prd(&prd, 0x100000, 65537), -1,
              "more than 64 KiB rejected");
}

/* -- 64 KiB boundary crossing -------------------------------------------- */
static void test_boundary_crossing(void)
{
    prdt_entry_t prd;

    /* Starts 256 bytes before a 64 KiB boundary, spans 512 -> crosses it. */
    ASSERT_EQ(ata_dma_build_prd(&prd, 0x0FFF00, 512), -1,
              "region straddling a 64 KiB boundary rejected");

    /* Ends exactly on the last byte before the boundary -> OK. */
    int r = ata_dma_build_prd(&prd, 0x0FFE00, 512);
    ASSERT_EQ(r, 0, "region ending at the 64 KiB boundary is valid");
    ASSERT_EQ((int)prd.byte_count, 512, "byte_count preserved");

    /* A 4 KiB page-aligned buffer (what the driver uses) never crosses. */
    ASSERT_EQ(ata_dma_build_prd(&prd, 0x0FF000, 4096), 0,
              "page-aligned 4 KiB region is valid");
}

int main(void)
{
    RUN_SUITE(test_basic_region);
    RUN_SUITE(test_full_64k);
    RUN_SUITE(test_bad_sizes);
    RUN_SUITE(test_boundary_crossing);
    TEST_SUMMARY();
}
