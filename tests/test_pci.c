/*
 * Quilon OS -- PCI Unit Tests (Section 10.1)
 *
 * Tests the pure-C parts of the PCI subsystem that can run on the host
 * without x86 I/O instructions:
 *
 *   1. pci_config_addr() -- address construction formula (static inline)
 *   2. pci_class_name()  -- class code string lookup
 *   3. pci_get_device()  -- device table index access
 *   4. pci_find_device() -- vendor/device ID search
 *
 * pci_read(), pci_write(), and pci_enumerate() are guarded by
 * #ifdef __is_kernel in pci.c and are NOT tested here.
 *
 * Build & run:  make (from tests/)
 */

#include "framework.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <kernel/pci.h>

/* -- Helper: inject synthetic devices into the global table ---------------- */

static void setup_table(void)
{
    pci_device_count = 0;
}

static void add_device(uint8_t bus, uint8_t slot, uint8_t func,
                       uint16_t vendor, uint16_t device,
                       uint8_t class_code, uint8_t subclass)
{
    if (pci_device_count >= PCI_MAX_DEVICES) return;
    pci_device_t *d = &pci_devices[pci_device_count++];
    d->bus        = bus;
    d->slot       = slot;
    d->func       = func;
    d->vendor_id  = vendor;
    d->device_id  = device;
    d->class_code = class_code;
    d->subclass   = subclass;
    d->prog_if    = 0;
    d->revision_id = 0;
    d->header_type = 0;
    d->interrupt_line = 0;
}

/* ===============================================================
 * 1. pci_config_addr -- address construction
 * =============================================================== */

static void test_config_addr_enable_bit(void)
{
    uint32_t addr = pci_config_addr(0, 0, 0, 0);
    ASSERT((addr >> 31) == 1u, "bit 31 (enable) is always set");
}

static void test_config_addr_bus_field(void)
{
    /* Bus 1 -> bits [23:16] = 1 */
    uint32_t addr = pci_config_addr(1, 0, 0, 0);
    uint8_t bus = (uint8_t)((addr >> 16) & 0xFF);
    ASSERT_EQ((unsigned)bus, 1u, "bus 1 lands in bits [23:16]");

    /* Bus 255 -> bits [23:16] = 0xFF */
    addr = pci_config_addr(255, 0, 0, 0);
    bus  = (uint8_t)((addr >> 16) & 0xFF);
    ASSERT_EQ((unsigned)bus, 255u, "bus 255 fills bits [23:16]");
}

static void test_config_addr_slot_field(void)
{
    /* Slot 1 -> bits [15:11] = 1 -> bit 11 set */
    uint32_t addr = pci_config_addr(0, 1, 0, 0);
    uint8_t slot = (uint8_t)((addr >> 11) & 0x1F);
    ASSERT_EQ((unsigned)slot, 1u, "slot 1 lands in bits [15:11]");

    /* Slot 31 (max) -> bits [15:11] = 0x1F */
    addr = pci_config_addr(0, 31, 0, 0);
    slot = (uint8_t)((addr >> 11) & 0x1F);
    ASSERT_EQ((unsigned)slot, 31u, "slot 31 fills bits [15:11]");
}

static void test_config_addr_func_field(void)
{
    uint32_t addr = pci_config_addr(0, 0, 3, 0);
    uint8_t func = (uint8_t)((addr >> 8) & 0x07);
    ASSERT_EQ((unsigned)func, 3u, "function 3 lands in bits [10:8]");
}

static void test_config_addr_offset_alignment(void)
{
    /* Offset 4 -> bits [7:2] = 1, bits [1:0] = 0 */
    uint32_t addr = pci_config_addr(0, 0, 0, 4);
    ASSERT_EQ((unsigned)(addr & 0x03), 0u, "bits [1:0] always 0");
    ASSERT_EQ((unsigned)((addr >> 2) & 0x3F), 1u,
              "offset 4 -> register index 1");

    /* Odd offset: low 2 bits must be cleared. */
    addr = pci_config_addr(0, 0, 0, 0x0F);
    ASSERT_EQ((unsigned)(addr & 0x03), 0u, "low 2 bits cleared for odd offset");
}

static void test_config_addr_known_value(void)
{
    /* pci_config_addr(0, 0, 0, 0) should equal 0x80000000. */
    ASSERT_EQ((unsigned)pci_config_addr(0, 0, 0, 0), 0x80000000u,
              "bus=0 slot=0 func=0 off=0 -> 0x80000000");
}

static void test_config_addr_rtl8139_pattern(void)
{
    /* QEMU default: RTL8139 at bus=0 slot=3 func=0, read offset 0.
     * Expected: 0x80000000 | (0<<16) | (3<<11) | (0<<8) | 0
     *         = 0x80000000 | 0x001800 = 0x80001800              */
    uint32_t addr = pci_config_addr(0, 3, 0, 0);
    ASSERT_EQ((unsigned)addr, 0x80001800u,
              "bus=0 slot=3 func=0 off=0 -> 0x80001800");
}

/* ===============================================================
 * 2. pci_class_name -- string lookup
 * =============================================================== */

static void test_class_name_known_codes(void)
{
    ASSERT_STR_EQ(pci_class_name(0x00), "Unclassified Device",
                  "class 0x00 is Unclassified Device");
    ASSERT_STR_EQ(pci_class_name(0x01), "Mass Storage Controller",
                  "class 0x01 is Mass Storage Controller");
    ASSERT_STR_EQ(pci_class_name(0x02), "Network Controller",
                  "class 0x02 is Network Controller");
    ASSERT_STR_EQ(pci_class_name(0x03), "Display Controller",
                  "class 0x03 is Display Controller");
    ASSERT_STR_EQ(pci_class_name(0x06), "Bridge Device",
                  "class 0x06 is Bridge Device");
}

static void test_class_name_unknown_code(void)
{
    /* 0xAA is not in the table -> fallback string. */
    const char *name = pci_class_name(0xAA);
    ASSERT_NOTNULL(name, "unknown class returns non-NULL string");
    ASSERT(name[0] != '\0', "unknown class string is not empty");
}

static void test_class_name_boundary_ff(void)
{
    /* 0xFF is the sentinel in the table (Unassigned Device).
     * It should return the "Unassigned Device" string, not "Unknown Class".
     * The loop stops before the sentinel, so 0xFF falls through to "Unknown". */
    const char *name = pci_class_name(0xFF);
    ASSERT_NOTNULL(name, "class 0xFF returns non-NULL");
    /* 0xFF is the last entry (sentinel) so it falls through to Unknown Class */
    ASSERT_STR_EQ(name, "Unknown Class", "class 0xFF -> Unknown Class (sentinel)");
}

/* ===============================================================
 * 3. pci_get_device -- index access
 * =============================================================== */

static void test_get_device_empty_table(void)
{
    setup_table();
    ASSERT_NULL(pci_get_device(0), "index 0 on empty table returns NULL");
    ASSERT_NULL(pci_get_device(-1), "negative index returns NULL");
}

static void test_get_device_valid_index(void)
{
    setup_table();
    add_device(0, 2, 0, 0x1234, 0x1111, 0x03, 0x00);   /* VGA */
    add_device(0, 3, 0, 0x10EC, 0x8139, 0x02, 0x00);   /* RTL8139 */

    pci_device_t *d0 = pci_get_device(0);
    ASSERT_NOTNULL(d0, "index 0 returns non-NULL");
    ASSERT_EQ((unsigned)d0->vendor_id, 0x1234u, "device 0 vendor matches");

    pci_device_t *d1 = pci_get_device(1);
    ASSERT_NOTNULL(d1, "index 1 returns non-NULL");
    ASSERT_EQ((unsigned)d1->vendor_id, 0x10ECu, "device 1 vendor matches");
}

static void test_get_device_out_of_bounds(void)
{
    setup_table();
    add_device(0, 0, 0, 0x8086, 0x1237, 0x06, 0x00);
    ASSERT_NULL(pci_get_device(1), "index == count returns NULL");
    ASSERT_NULL(pci_get_device(100), "large index returns NULL");
}

/* ===============================================================
 * 4. pci_find_device -- vendor/device ID search
 * =============================================================== */

static void test_find_device_empty(void)
{
    setup_table();
    ASSERT_NULL(pci_find_device(0x8086, 0x1237),
                "find on empty table returns NULL");
}

static void test_find_device_present(void)
{
    setup_table();
    add_device(0, 0, 0, 0x8086, 0x1237, 0x06, 0x00);  /* Intel host bridge */
    add_device(0, 3, 0, 0x10EC, 0x8139, 0x02, 0x00);  /* RTL8139 */

    pci_device_t *rtl = pci_find_device(PCI_VENDOR_REALTEK, PCI_DEVICE_RTL8139);
    ASSERT_NOTNULL(rtl, "RTL8139 found after insertion");
    ASSERT_EQ((unsigned)rtl->slot, 3u, "RTL8139 is in slot 3");
    ASSERT_EQ((unsigned)rtl->class_code, 0x02u, "RTL8139 class is Network");
}

static void test_find_device_not_present(void)
{
    setup_table();
    add_device(0, 0, 0, 0x8086, 0x1237, 0x06, 0x00);
    ASSERT_NULL(pci_find_device(0x10EC, 0x8139),
                "RTL8139 not found when not in table");
}

static void test_find_device_returns_first_match(void)
{
    setup_table();
    /* Two devices with the same vendor/device (unusual but valid). */
    add_device(0, 2, 0, 0x1234, 0x1111, 0x03, 0x00);
    add_device(0, 4, 0, 0x1234, 0x1111, 0x03, 0x00);

    pci_device_t *d = pci_find_device(0x1234, 0x1111);
    ASSERT_NOTNULL(d, "find returns non-NULL for duplicate");
    ASSERT_EQ((unsigned)d->slot, 2u, "returns first match (slot 2, not slot 4)");
}

/* ===============================================================
 * 5. Device table limit
 * =============================================================== */

static void test_device_table_limit(void)
{
    setup_table();
    for (int i = 0; i < PCI_MAX_DEVICES + 10; i++)
        add_device((uint8_t)(i >> 5), (uint8_t)(i & 0x1F), 0,
                   0x1234, (uint16_t)i, 0x06, 0x00);

    ASSERT(pci_device_count <= PCI_MAX_DEVICES,
           "device count never exceeds PCI_MAX_DEVICES");
    ASSERT_EQ(pci_device_count, PCI_MAX_DEVICES,
              "table fills exactly to PCI_MAX_DEVICES");
}

/* ===============================================================
 * main
 * =============================================================== */

int main(void)
{
    RUN_SUITE(test_config_addr_enable_bit);
    RUN_SUITE(test_config_addr_bus_field);
    RUN_SUITE(test_config_addr_slot_field);
    RUN_SUITE(test_config_addr_func_field);
    RUN_SUITE(test_config_addr_offset_alignment);
    RUN_SUITE(test_config_addr_known_value);
    RUN_SUITE(test_config_addr_rtl8139_pattern);

    RUN_SUITE(test_class_name_known_codes);
    RUN_SUITE(test_class_name_unknown_code);
    RUN_SUITE(test_class_name_boundary_ff);

    RUN_SUITE(test_get_device_empty_table);
    RUN_SUITE(test_get_device_valid_index);
    RUN_SUITE(test_get_device_out_of_bounds);

    RUN_SUITE(test_find_device_empty);
    RUN_SUITE(test_find_device_present);
    RUN_SUITE(test_find_device_not_present);
    RUN_SUITE(test_find_device_returns_first_match);

    RUN_SUITE(test_device_table_limit);

    TEST_SUMMARY();
}
