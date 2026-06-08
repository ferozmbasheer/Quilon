#ifndef _KERNEL_ATA_DMA_H
#define _KERNEL_ATA_DMA_H

/*
 * Quilon OS -- ATA Bus Master DMA (Section 16.1)
 *
 * The PIO driver (ata.c) copies every 512-byte sector word-by-word with the
 * CPU.  Bus Master DMA instead hands the IDE controller a Physical Region
 * Descriptor Table (PRDT) describing where in memory to put the data, then the
 * controller copies it directly and raises IRQ14 when done -- the CPU only sets
 * up the transfer and waits.
 *
 * The Bus Master registers live at PCI BAR4 of the IDE controller (an I/O BAR).
 * For the primary channel:
 *   BAR4 + 0  Command register  (bit 0 = start/stop, bit 3 = read/write dir)
 *   BAR4 + 2  Status register   (bit 0 = active, bit 1 = error, bit 2 = IRQ)
 *   BAR4 + 4  PRDT address      (32-bit physical address of the PRDT)
 *
 * The pure PRDT-building logic (ata_dma_build_prd) is a static inline so the
 * host unit test can exercise it without any x86 I/O -- see tests/test_ata_dma.c.
 */

#include <stdint.h>

/* -- Physical Region Descriptor ------------------------------------------- *
 * One entry describes a physically-contiguous buffer for the controller to
 * DMA into / out of.  The table is an array of these; bit 15 of `flags` marks
 * the last entry (End Of Table).  A region may not cross a 64 KiB physical
 * boundary, and byte_count == 0 means 65536 bytes.                          */
typedef struct {
    uint32_t phys_addr;   /* physical base address of the buffer            */
    uint16_t byte_count;  /* transfer size in bytes (0 == 65536)            */
    uint16_t flags;       /* bit 15 (PRDT_EOT) set on the final entry       */
} __attribute__((packed)) prdt_entry_t;

#define PRDT_EOT          0x8000u   /* "end of table" flag in prdt_entry.flags */
#define PRDT_MAX_BYTES    65536u    /* one region covers at most 64 KiB        */

/* -- Bus Master register offsets (relative to BAR4 channel base) ----------- */
#define BM_REG_COMMAND    0x0
#define BM_REG_STATUS     0x2
#define BM_REG_PRDT       0x4

/* Bus Master Command register bits */
#define BM_CMD_START      0x01   /* 1 = start engine, 0 = stop              */
#define BM_CMD_WRITE      0x08   /* direction: 1 = device->memory (read)    */

/* Bus Master Status register bits */
#define BM_SR_ACTIVE      0x01   /* transfer in progress                    */
#define BM_SR_ERROR       0x02   /* DMA error                               */
#define BM_SR_INTERRUPT   0x04   /* device raised its IRQ                    */

/* -- Pure helper (host-testable) ------------------------------------------ *
 * Fill a single-entry PRDT covering `bytes` at physical address `phys`,
 * marked End-Of-Table.  Returns 0 on success, -1 if the region is invalid:
 *   - bytes == 0 or bytes > 64 KiB, or
 *   - the region would cross a 64 KiB physical boundary.
 * byte_count is stored as `bytes` (a full 64 KiB is encoded as 0).          */
static inline int ata_dma_build_prd(prdt_entry_t *prd, uint32_t phys,
                                    uint32_t bytes)
{
    if (bytes == 0 || bytes > PRDT_MAX_BYTES)
        return -1;

    /* A PRD region must not straddle a 64 KiB boundary: the controller's
     * byte counter only carries within the low 16 bits of the address. */
    if ((phys & ~0xFFFFu) != ((phys + bytes - 1) & ~0xFFFFu))
        return -1;

    prd->phys_addr  = phys;
    prd->byte_count = (uint16_t)(bytes & 0xFFFFu);   /* 64 KiB wraps to 0 */
    prd->flags      = PRDT_EOT;
    return 0;
}

/* -- Hardware API (kernel only) ------------------------------------------- */

/* Probe PCI for a bus-master-capable IDE controller, read BAR4, enable PCI
 * bus mastering, and prepare the PRDT + bounce buffer.  Must be called after
 * pci_enumerate().  Returns 0 on success, -1 if no usable controller was found
 * (the driver then stays on PIO).                                           */
int ata_dma_init(void);

/* Returns 1 if ata_dma_init() succeeded and DMA transfers are usable. */
int ata_dma_available(void);

/* Read/write `count` 512-byte sectors via Bus Master DMA.  Same contract as
 * the PIO ata_read_sectors/ata_write_sectors.  Returns 0 on success, -1 on
 * error (the caller may retry the operation in PIO mode).                   */
int ata_dma_read(int drive, uint32_t lba, uint32_t count, void *buf);
int ata_dma_write(int drive, uint32_t lba, uint32_t count, const void *buf);

/* IRQ14 completion hook -- call from irq14_handler before sending the EOI.
 * Reads/clears the controller's interrupt state so the line deasserts.      */
void ata_dma_irq(void);

#endif /* _KERNEL_ATA_DMA_H */
