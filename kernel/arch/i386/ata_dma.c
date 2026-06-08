/*
 * Quilon OS -- ATA Bus Master DMA Driver (Section 16.1)
 *
 * Replaces the per-word CPU copy of the PIO driver (ata.c) with hardware DMA:
 * the IDE controller copies sectors to/from memory itself, guided by a Physical
 * Region Descriptor Table (PRDT), and raises IRQ14 on completion.
 *
 * Memory model note
 * -----------------
 * DMA hardware needs a *physical* address, and the region must be physically
 * contiguous.  Rather than translate arbitrary caller buffers (which may live
 * above the 4 MiB identity map and be reachable only via kmap), we DMA into a
 * fixed, page-aligned *bounce buffer* in the kernel BSS -- which is in the first
 * few MiB of physical memory, so its physical address is simply
 * `virt - KERNEL_OFFSET`.  Reads copy bounce->caller afterwards; writes copy
 * caller->bounce first.  A 4 KiB page-aligned buffer never crosses a 64 KiB
 * boundary, satisfying the PRD constraint.  The buffer caps one DMA op at
 * 8 sectors; larger transfers loop.  FAT16 reads one sector at a time, so this
 * is ample.
 *
 * Completion
 * ----------
 * Networking in Quilon is polled, and DMA can in principle run before the
 * scheduler exists, so we don't block a process on a waitq.  Instead the IRQ14
 * hook (ata_dma_irq) clears the controller's interrupt and sets a flag, and the
 * issuing thread busy-polls that flag OR the Bus Master "active" bit (with a
 * timeout).  Both the IRQ path and the poll path clear the controller state, so
 * the driver is correct whether or not the interrupt is delivered.
 */

#include <stdint.h>
#include <string.h>

#include <kernel/ata.h>
#include <kernel/ata_dma.h>
#include <kernel/pci.h>
#include <kernel/paging.h>   /* KERNEL_OFFSET */

/* -- Primary ATA bus port addresses (shared with the PIO driver) ----------- */
#define ATA_DATA          0x1F0
#define ATA_SECTOR_COUNT  0x1F2
#define ATA_LBA_LO        0x1F3
#define ATA_LBA_MID       0x1F4
#define ATA_LBA_HI        0x1F5
#define ATA_DRIVE_HEAD    0x1F6
#define ATA_STATUS        0x1F7   /* read = status, write = command */
#define ATA_DEV_CONTROL   0x3F6   /* write: bit 1 (nIEN) masks the IRQ */
#define ATA_ALT_STATUS    0x3F6   /* read: status without clearing IRQ */

#define ATA_SR_BSY        0x80
#define ATA_SR_DRDY       0x40
#define ATA_SR_DF         0x20    /* drive fault */
#define ATA_SR_ERR        0x01

#define ATA_CMD_READ_DMA  0xC8
#define ATA_CMD_WRITE_DMA 0xCA

/* PCI class for a mass-storage IDE controller. */
#define PCI_CLASS_STORAGE 0x01
#define PCI_SUBCLASS_IDE  0x01
#define PCI_CMD_BUS_MASTER 0x04   /* command register bit 2 */

/* One DMA op moves at most this many bytes (the bounce buffer size). */
#define DMA_CHUNK_BYTES   4096u
#define DMA_CHUNK_SECTORS (DMA_CHUNK_BYTES / ATA_SECTOR_SIZE)   /* 8 */

#define DMA_POLL_LIMIT    2000000   /* busy-poll iterations before timeout */

/* -- Port I/O -------------------------------------------------------------- */
static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    asm volatile("inb %w1, %b0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outb(uint16_t port, uint8_t v)
{
    asm volatile("outb %b0, %w1" :: "a"(v), "Nd"(port));
}
static inline void outl(uint16_t port, uint32_t v)
{
    asm volatile("outl %0, %w1" :: "a"(v), "Nd"(port));
}

/* -- Driver state ---------------------------------------------------------- */
static uint16_t bm_base;          /* Bus Master I/O base (primary channel) */
static int      dma_ready;        /* 1 once init succeeds                   */
static volatile int dma_irq_fired;

/* Bounce buffer + PRDT live in kernel BSS (first few MiB of phys memory) so
 * their physical address is virt - KERNEL_OFFSET.  Page-aligned so the bounce
 * buffer never crosses a 64 KiB boundary and the PRDT is well-formed.       */
static uint8_t      dma_buf[DMA_CHUNK_BYTES] __attribute__((aligned(4096)));
static prdt_entry_t dma_prdt[1]              __attribute__((aligned(4096)));

static inline uint32_t virt_to_phys(const void *p)
{
    return (uint32_t)(uintptr_t)p - KERNEL_OFFSET;
}

/* -- Polling helper -------------------------------------------------------- */
static int ata_wait_not_busy(void)
{
    for (int i = 0; i < 100000; i++)
        if (!(inb(ATA_STATUS) & ATA_SR_BSY))
            return 0;
    return -1;
}

/* -- Init ------------------------------------------------------------------ */
int ata_dma_init(void)
{
    /* Find an IDE controller (class 0x01, subclass 0x01) in the table that
     * pci_enumerate() populated.                                            */
    pci_device_t *ide = NULL;
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].class_code == PCI_CLASS_STORAGE &&
            pci_devices[i].subclass   == PCI_SUBCLASS_IDE) {
            ide = &pci_devices[i];
            break;
        }
    }
    if (!ide)
        return -1;

    /* BAR4 holds the Bus Master base.  It is an I/O BAR (bit 0 = 1); the base
     * address is bits [15:2].                                               */
    uint32_t bar4 = pci_read(ide->bus, ide->slot, ide->func, PCI_OFF_BAR4);
    if (!(bar4 & 0x1))
        return -1;                       /* memory-mapped BAR -- unexpected */
    bm_base = (uint16_t)(bar4 & 0xFFFCu);
    if (bm_base == 0)
        return -1;

    /* Enable PCI bus mastering so the controller may drive the bus. */
    uint32_t cmd = pci_read(ide->bus, ide->slot, ide->func, PCI_OFF_COMMAND);
    if (!(cmd & PCI_CMD_BUS_MASTER)) {
        cmd |= PCI_CMD_BUS_MASTER;
        pci_write(ide->bus, ide->slot, ide->func, PCI_OFF_COMMAND, cmd);
    }

    /* Make sure device interrupts are enabled (nIEN = 0) so IRQ14 fires. */
    outb(ATA_DEV_CONTROL, 0x00);

    /* Stop the engine and clear any stale interrupt/error status. */
    outb(bm_base + BM_REG_COMMAND, 0x00);
    outb(bm_base + BM_REG_STATUS, BM_SR_INTERRUPT | BM_SR_ERROR);

    dma_ready = 1;
    return 0;
}

int ata_dma_available(void)
{
    return dma_ready;
}

/* -- IRQ14 completion hook ------------------------------------------------- */
void ata_dma_irq(void)
{
    if (!dma_ready)
        return;
    uint8_t st = inb(bm_base + BM_REG_STATUS);
    if (!(st & BM_SR_INTERRUPT))
        return;                          /* not a DMA completion */
    inb(ATA_STATUS);                     /* read ATA status -> deassert IRQ */
    outb(bm_base + BM_REG_STATUS, BM_SR_INTERRUPT | BM_SR_ERROR);  /* W1C */
    dma_irq_fired = 1;
}

/* -- Single-chunk transfer ------------------------------------------------- *
 * Programs one DMA of `sectors` (<= DMA_CHUNK_SECTORS) sectors at `lba` on
 * `drive`.  `is_write` selects direction.  Data always flows through the
 * bounce buffer (already filled by the caller for writes).  Returns 0 / -1.  */
static int dma_chunk(int drive, uint32_t lba, uint32_t sectors, int is_write)
{
    uint32_t bytes = sectors * ATA_SECTOR_SIZE;

    if (ata_dma_build_prd(&dma_prdt[0], virt_to_phys(dma_buf), bytes) != 0)
        return -1;

    if (ata_wait_not_busy() < 0)
        return -1;

    /* 1. Stop the engine; clear stale interrupt/error bits. */
    outb(bm_base + BM_REG_COMMAND, 0x00);
    outb(bm_base + BM_REG_STATUS, BM_SR_INTERRUPT | BM_SR_ERROR);

    /* 2. Point the controller at the PRDT (physical address). */
    outl(bm_base + BM_REG_PRDT, virt_to_phys(dma_prdt));

    /* 3. Set the transfer direction (without starting yet).
     *    BM_CMD_WRITE means device->memory, i.e. a disk READ.              */
    outb(bm_base + BM_REG_COMMAND, is_write ? 0x00 : BM_CMD_WRITE);

    /* 4. Select drive and program the 28-bit LBA + sector count. */
    outb(ATA_DRIVE_HEAD,
         (uint8_t)(0xE0 | ((drive & 1) << 4) | ((lba >> 24) & 0x0F)));
    outb(ATA_SECTOR_COUNT, (uint8_t)(sectors & 0xFF));
    outb(ATA_LBA_LO,  (uint8_t)( lba        & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >>  8) & 0xFF));
    outb(ATA_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));

    /* 5. Issue the DMA command. */
    dma_irq_fired = 0;
    outb(ATA_STATUS, is_write ? ATA_CMD_WRITE_DMA : ATA_CMD_READ_DMA);

    /* 6. Start the bus master engine (preserve the direction bit). */
    outb(bm_base + BM_REG_COMMAND,
         BM_CMD_START | (is_write ? 0x00 : BM_CMD_WRITE));

    /* 7. Wait for completion: IRQ flag, or the engine clearing "active". */
    int timed_out = 1;
    for (uint32_t i = 0; i < DMA_POLL_LIMIT; i++) {
        if (dma_irq_fired) { timed_out = 0; break; }
        uint8_t bm = inb(bm_base + BM_REG_STATUS);
        if (bm & BM_SR_ERROR)            break;   /* DMA error */
        if (!(bm & BM_SR_ACTIVE))        { timed_out = 0; break; }
    }

    /* 8. Stop the engine and read final status (also clears IRQ if the
     *    handler never ran, e.g. interrupts were masked). */
    outb(bm_base + BM_REG_COMMAND, 0x00);
    uint8_t bm_status  = inb(bm_base + BM_REG_STATUS);
    uint8_t ata_status = inb(ATA_STATUS);
    outb(bm_base + BM_REG_STATUS, BM_SR_INTERRUPT | BM_SR_ERROR);

    if (timed_out)
        return -1;
    if (bm_status & BM_SR_ERROR)
        return -1;
    if (ata_status & (ATA_SR_ERR | ATA_SR_DF))
        return -1;
    return 0;
}

/* -- Public read/write ----------------------------------------------------- */
int ata_dma_read(int drive, uint32_t lba, uint32_t count, void *buf)
{
    if (!dma_ready || drive < 0 || drive > 1 || !ata_drive_present(drive))
        return -1;
    if (count == 0)
        return 0;

    uint8_t *out = (uint8_t *)buf;
    while (count > 0) {
        uint32_t n = count < DMA_CHUNK_SECTORS ? count : DMA_CHUNK_SECTORS;
        if (dma_chunk(drive, lba, n, /*is_write=*/0) != 0)
            return -1;
        memcpy(out, dma_buf, n * ATA_SECTOR_SIZE);
        out   += n * ATA_SECTOR_SIZE;
        lba   += n;
        count -= n;
    }
    return 0;
}

int ata_dma_write(int drive, uint32_t lba, uint32_t count, const void *buf)
{
    if (!dma_ready || drive < 0 || drive > 1 || !ata_drive_present(drive))
        return -1;
    if (count == 0)
        return 0;

    const uint8_t *in = (const uint8_t *)buf;
    while (count > 0) {
        uint32_t n = count < DMA_CHUNK_SECTORS ? count : DMA_CHUNK_SECTORS;
        memcpy(dma_buf, in, n * ATA_SECTOR_SIZE);
        if (dma_chunk(drive, lba, n, /*is_write=*/1) != 0)
            return -1;
        in    += n * ATA_SECTOR_SIZE;
        lba   += n;
        count -= n;
    }
    return 0;
}
