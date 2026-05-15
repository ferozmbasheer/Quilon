/*
 * Quilon OS -- PCI Bus Enumeration (Section 10.1)
 *
 * PCI configuration space is accessed via two 32-bit I/O ports:
 *   0xCF8  CONFIG_ADDRESS -- write to select which register to access
 *   0xCFC  CONFIG_DATA    -- read/write the selected 32-bit DWORD
 *
 * The address word written to CONFIG_ADDRESS:
 *   Bit  31    : Enable bit (must be 1)
 *   Bits 30:24 : Reserved (0)
 *   Bits 23:16 : PCI bus number   (0–255)
 *   Bits 15:11 : Device / slot    (0–31)
 *   Bits 10:8  : Function number  (0–7)
 *   Bits  7:2  : DWORD register index (offset >> 2)
 *   Bits  1:0  : Always 0 (DWORD-aligned access only)
 */

#ifndef _KERNEL_PCI_H
#define _KERNEL_PCI_H

#include <stdint.h>

/* -- I/O port addresses ---------------------------------------------------- */
#define PCI_CONFIG_ADDR  0xCF8u
#define PCI_CONFIG_DATA  0xCFCu

/* -- Configuration space byte offsets ------------------------------------- */
#define PCI_OFF_VENDOR_ID    0x00   /* [15:0]  vendor,  [31:16] device      */
#define PCI_OFF_COMMAND      0x04   /* [15:0]  command, [31:16] status       */
#define PCI_OFF_REVISION_ID  0x08   /* [7:0]   rev, [15:8] prog_if,          */
                                    /* [23:16] subclass, [31:24] class_code  */
#define PCI_OFF_CACHE_LINE   0x0C   /* [7:0]   cache_line, [15:8] lat_timer, */
                                    /* [23:16] header_type, [31:24] BIST     */
#define PCI_OFF_BAR0         0x10
#define PCI_OFF_BAR1         0x14
#define PCI_OFF_BAR2         0x18
#define PCI_OFF_BAR3         0x1C
#define PCI_OFF_BAR4         0x20
#define PCI_OFF_BAR5         0x24
#define PCI_OFF_INT_LINE     0x3C   /* [7:0] interrupt line, [15:8] int pin  */

/* -- Header type flags ----------------------------------------------------- */
#define PCI_HEADER_MULTIFUNC  0x80u  /* bit 7: multiple functions present */

/* -- Sentinel -------------------------------------------------------------- */
#define PCI_VENDOR_NONE       0xFFFFu  /* slot empty */

/* -- Well-known vendors ---------------------------------------------------- */
#define PCI_VENDOR_INTEL      0x8086u
#define PCI_VENDOR_REALTEK    0x10ECu
#define PCI_VENDOR_AMD        0x1022u
#define PCI_VENDOR_VMWARE     0x15ADu
#define PCI_VENDOR_QEMU       0x1234u  /* QEMU/Bochs virtual VGA */

/* -- Well-known device IDs ------------------------------------------------- */
#define PCI_DEVICE_RTL8139    0x8139u  /* Realtek RTL8139 NIC (section 10.2) */

/* -- Device table limit ---------------------------------------------------- */
#define PCI_MAX_DEVICES  64

/* -- Device descriptor ----------------------------------------------------- */
typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision_id;
    uint8_t  header_type;
    uint8_t  interrupt_line;
} pci_device_t;

/* -- Global device table (populated by pci_enumerate) ---------------------- */
extern pci_device_t pci_devices[PCI_MAX_DEVICES];
extern int          pci_device_count;

/* -- Address construction (pure C, usable on host for tests) --------------- */

/* Build the 32-bit value for PCI_CONFIG_ADDR.
 * offset low 2 bits are masked to 0 (DWORD-aligned access required).        */
static inline uint32_t pci_config_addr(uint8_t bus, uint8_t slot,
                                        uint8_t func, uint8_t offset)
{
    return (1u << 31)
         | ((uint32_t)bus  << 16)
         | ((uint32_t)slot << 11)
         | ((uint32_t)func <<  8)
         | ((uint32_t)offset & 0xFCu);
}

/* -- API ------------------------------------------------------------------- */

/* Read a 32-bit DWORD from PCI configuration space. */
uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

/* Write a 32-bit DWORD to PCI configuration space. */
void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
               uint32_t value);

/* Enumerate all PCI buses (0–255) and slots (0–31).
 * Populates pci_devices[] and sets pci_device_count.
 * Safe to call multiple times; resets the table each call.                  */
void pci_enumerate(void);

/* Return the first device whose vendor_id and device_id match, or NULL.     */
pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id);

/* Return the Nth enumerated device, or NULL if index >= pci_device_count.   */
pci_device_t *pci_get_device(int index);

/* Return a human-readable string for a PCI class code byte.                 */
const char *pci_class_name(uint8_t class_code);

#endif /* _KERNEL_PCI_H */
