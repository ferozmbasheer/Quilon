/*
 * Quilon OS — PCI Bus Enumeration (Section 10.1)
 *
 * PCI Configuration Access Mechanism #1 uses two I/O ports:
 *
 *   0xCF8  CONFIG_ADDRESS — 32-bit write to select bus/slot/func/register
 *   0xCFC  CONFIG_DATA    — 32-bit read/write of the selected register
 *
 * Scanning strategy
 * ──────────────────
 * For each (bus, slot) pair we read offset 0x00, which returns the vendor
 * and device IDs in a single 32-bit read.  If the low 16 bits are 0xFFFF,
 * no device occupies that slot and we skip it.
 *
 * Multi-function devices set bit 7 of the Header Type byte (offset 0x0E,
 * inside the DWORD at 0x0C).  When set, functions 1–7 may each be an
 * independent device and must be probed individually.
 *
 * Hardware-dependent functions (pci_read, pci_write, pci_enumerate) are
 * guarded by #ifdef __is_kernel so that this file compiles on the host
 * for unit testing without touching any x86 I/O instructions.
 */

#include <stdint.h>
#include <stdio.h>
#include <kernel/pci.h>

/* ── Global device table ─────────────────────────────────────────────────── */

pci_device_t pci_devices[PCI_MAX_DEVICES];
int          pci_device_count = 0;

/* ── Class code name table ───────────────────────────────────────────────── */

static const struct {
    uint8_t     code;
    const char *name;
} pci_class_table[] = {
    { 0x00, "Unclassified Device"           },
    { 0x01, "Mass Storage Controller"       },
    { 0x02, "Network Controller"            },
    { 0x03, "Display Controller"            },
    { 0x04, "Multimedia Controller"         },
    { 0x05, "Memory Controller"             },
    { 0x06, "Bridge Device"                 },
    { 0x07, "Communication Controller"      },
    { 0x08, "System Peripheral"             },
    { 0x09, "Input Device Controller"       },
    { 0x0A, "Docking Station"               },
    { 0x0B, "Processor"                     },
    { 0x0C, "Serial Bus Controller"         },
    { 0x0D, "Wireless Controller"           },
    { 0x0E, "Intelligent I/O Controller"    },
    { 0x0F, "Satellite Communication"       },
    { 0x10, "Encryption Controller"         },
    { 0x11, "Signal Processing Controller"  },
    { 0xFF, "Unassigned Device"             },
};

#define CLASS_TABLE_LEN \
    (int)(sizeof(pci_class_table) / sizeof(pci_class_table[0]))

const char *pci_class_name(uint8_t class_code)
{
    for (int i = 0; i < CLASS_TABLE_LEN - 1; i++) {
        if (pci_class_table[i].code == class_code)
            return pci_class_table[i].name;
    }
    return "Unknown Class";
}

/* ── Device table queries ────────────────────────────────────────────────── */

pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id)
{
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor_id &&
            pci_devices[i].device_id == device_id)
            return &pci_devices[i];
    }
    return (pci_device_t *)0;
}

pci_device_t *pci_get_device(int index)
{
    if (index < 0 || index >= pci_device_count)
        return (pci_device_t *)0;
    return &pci_devices[index];
}

/* ── Hardware I/O — x86 only, excluded from host test builds ─────────────── */

#ifdef __is_kernel

static inline void outl_port(uint16_t port, uint32_t value)
{
    asm volatile("outl %0, %w1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl_port(uint16_t port)
{
    uint32_t v;
    asm volatile("inl %w1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    outl_port(PCI_CONFIG_ADDR, pci_config_addr(bus, slot, func, offset));
    return inl_port(PCI_CONFIG_DATA);
}

void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
               uint32_t value)
{
    outl_port(PCI_CONFIG_ADDR, pci_config_addr(bus, slot, func, offset));
    outl_port(PCI_CONFIG_DATA, value);
}

/* Record one function from the bus scan into pci_devices[]. */
static void pci_scan_function(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint32_t id_word = pci_read(bus, slot, func, PCI_OFF_VENDOR_ID);
    if ((id_word & 0xFFFF) == PCI_VENDOR_NONE) return;
    if (pci_device_count >= PCI_MAX_DEVICES) return;

    /* Offset 0x08: [7:0]=revision, [15:8]=prog_if, [23:16]=subclass, [31:24]=class */
    uint32_t class_word = pci_read(bus, slot, func, PCI_OFF_REVISION_ID);
    /* Offset 0x0C: [7:0]=cache_line, [15:8]=lat_timer, [23:16]=header_type, [31:24]=BIST */
    uint32_t cache_word = pci_read(bus, slot, func, PCI_OFF_CACHE_LINE);
    /* Offset 0x3C: [7:0]=int_line, [15:8]=int_pin */
    uint32_t int_word   = pci_read(bus, slot, func, PCI_OFF_INT_LINE);

    pci_device_t *d = &pci_devices[pci_device_count++];
    d->bus            = bus;
    d->slot           = slot;
    d->func           = func;
    d->vendor_id      = (uint16_t)(id_word        & 0xFFFF);
    d->device_id      = (uint16_t)(id_word  >> 16);
    d->revision_id    = (uint8_t) (class_word       & 0xFF);
    d->prog_if        = (uint8_t)((class_word >>  8) & 0xFF);
    d->subclass       = (uint8_t)((class_word >> 16) & 0xFF);
    d->class_code     = (uint8_t)((class_word >> 24) & 0xFF);
    d->header_type    = (uint8_t)((cache_word >> 16) & 0xFF);
    d->interrupt_line = (uint8_t)(int_word            & 0xFF);
}

/* Probe one slot; if it has multiple functions, probe each one. */
static void pci_scan_slot(uint8_t bus, uint8_t slot)
{
    uint32_t id_word = pci_read(bus, slot, 0, PCI_OFF_VENDOR_ID);
    if ((id_word & 0xFFFF) == PCI_VENDOR_NONE) return;

    pci_scan_function(bus, slot, 0);

    /* Header type bit 7 set → multi-function device; check functions 1–7. */
    uint32_t cache_word = pci_read(bus, slot, 0, PCI_OFF_CACHE_LINE);
    uint8_t  htype      = (uint8_t)((cache_word >> 16) & 0xFF);
    if (htype & PCI_HEADER_MULTIFUNC) {
        for (uint8_t func = 1; func < 8; func++)
            pci_scan_function(bus, slot, func);
    }
}

void pci_enumerate(void)
{
    pci_device_count = 0;
    for (int bus = 0; bus < 256; bus++)
        for (int slot = 0; slot < 32; slot++)
            pci_scan_slot((uint8_t)bus, (uint8_t)slot);
}

#endif /* __is_kernel */
