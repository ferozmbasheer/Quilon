/*
 * Quilon OS -- ATA PIO Driver (Primary Bus)
 *
 * Implements 28-bit LBA PIO (Programmed I/O) mode.  All I/O is performed by
 * polling status registers rather than using DMA or interrupts.  This is the
 * simplest possible ATA implementation and is good enough for a hobby OS.
 *
 * Primary ATA bus port map
 * ------------------------
 *   0x1F0  Data register      -- 16-bit; read/write 256 words per sector
 *   0x1F1  Error (read) / Features (write)
 *   0x1F2  Sector count
 *   0x1F3  LBA bits  7:0
 *   0x1F4  LBA bits 15:8
 *   0x1F5  LBA bits 23:16
 *   0x1F6  Drive/Head         -- [7]=1, [6]=LBA, [5]=1, [4]=drive#, [3:0]=LBA 27:24
 *   0x1F7  Status (read) / Command (write)
 *   0x3F6  Alternate status / device control
 *
 * Drive/Head register (0x1F6) for 28-bit LBA
 *   Bits 7, 5 are always 1 (legacy OBS bits).
 *   Bit 6 = 1 selects LBA addressing (as opposed to CHS).
 *   Bit 4 = 0 -> master, 1 -> slave.
 *   Bits 3:0 = LBA bits 27:24.
 *   So the base value for LBA mode, master = 0b11100000 = 0xE0.
 */

#include <stdint.h>
#include <kernel/ata.h>
#include <kernel/ata_dma.h>
#include <kernel/spinlock.h>

/* -- Primary bus port addresses -------------------------------------------- */
#define ATA_DATA         0x1F0
#define ATA_ERROR        0x1F1
#define ATA_SECTOR_COUNT 0x1F2
#define ATA_LBA_LO       0x1F3
#define ATA_LBA_MID      0x1F4
#define ATA_LBA_HI       0x1F5
#define ATA_DRIVE_HEAD   0x1F6
#define ATA_STATUS       0x1F7   /* read  = status, write = command */
#define ATA_ALT_STATUS   0x3F6   /* reading this does NOT clear interrupts */

/* Status register bit masks */
#define ATA_SR_BSY   0x80   /* controller busy                        */
#define ATA_SR_DRDY  0x40   /* drive ready                            */
#define ATA_SR_DRQ   0x08   /* data request -- drive has data for us   */
#define ATA_SR_ERR   0x01   /* error flag                             */

/* Command codes */
#define ATA_CMD_READ_SECTORS   0x20   /* read up to 255 sectors, PIO  */
#define ATA_CMD_WRITE_SECTORS  0x30   /* write up to 255 sectors, PIO */
#define ATA_CMD_IDENTIFY       0xEC   /* identify drive               */

static int drive_present[2] = {0, 0};

/* -- Inline port I/O ------------------------------------------------------- */
/*
 * Inline helpers avoid function-call overhead on the hot path (256 inw calls
 * per sector) while keeping the assembly close to the usage site.
 */

static inline uint8_t ata_inb(uint16_t port)
{
    uint8_t val;
    asm volatile("inb %w1, %b0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void ata_outb(uint16_t port, uint8_t val)
{
    asm volatile("outb %b0, %w1" :: "a"(val), "Nd"(port));
}

static inline uint16_t ata_inw(uint16_t port)
{
    uint16_t val;
    asm volatile("inw %w1, %w0" : "=a"(val) : "Nd"(port));
    return val;
}

/* -- Polling helpers ------------------------------------------------------- */

/*
 * ata_400ns_delay -- read the alternate status register four times.
 *
 * Each read takes ~100 ns on ISA-speed hardware, giving ~400 ns total.
 * Required after selecting a drive before reading the status register, so
 * the drive has time to assert BSY.
 */
static void ata_400ns_delay(void)
{
    for (int i = 0; i < 4; i++)
        ata_inb(ATA_ALT_STATUS);
}

/* Spin until BSY clears.  Returns 0 on success, -1 on timeout. */
static int ata_wait_not_busy(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(ata_inb(ATA_STATUS) & ATA_SR_BSY))
            return 0;
    }
    return -1;
}

/* Spin until DRQ or ERR is set.  Returns 0 on DRQ, -1 on ERR or timeout. */
static int ata_wait_drq(void)
{
    for (int i = 0; i < 100000; i++) {
        uint8_t status = ata_inb(ATA_STATUS);
        if (status & ATA_SR_ERR)  return -1;
        if (status & ATA_SR_DRQ)  return  0;
    }
    return -1;
}

/* -- Public API ------------------------------------------------------------ */

int ata_initialize(void)
{
    int found = 0;

    for (int drv = 0; drv <= 1; drv++) {
        /*
         * Select the drive.  The drive select bit (bit 4) chooses master (0)
         * or slave (1).  Bits 7, 5 and 6 (LBA mode) are set to 1.
         * We write 0xA0 | (drv << 4) here -- LBA bit 6 not set yet, that
         * is fine for IDENTIFY which ignores LBA bits.
         */
        ata_outb(ATA_DRIVE_HEAD, (uint8_t)(0xA0 | (drv << 4)));
        ata_400ns_delay();

        /* Issue IDENTIFY command */
        ata_outb(ATA_STATUS, ATA_CMD_IDENTIFY);

        /* An all-zero status byte means no device is present on this position */
        if (ata_inb(ATA_STATUS) == 0)
            continue;

        if (ata_wait_not_busy() < 0)
            continue;

        /* Consume the 256-word IDENTIFY response to empty the data register */
        if (ata_wait_drq() == 0) {
            for (int w = 0; w < 256; w++)
                ata_inw(ATA_DATA);   /* discard; we don't need the IDENTIFY data */
            drive_present[drv] = 1;
            found++;
        }
    }

    return found;
}

int ata_drive_present(int drive)
{
    if (drive < 0 || drive > 1) return 0;
    return drive_present[drive];
}

/* The ATA PIO transaction (select drive, program LBA, poll status, transfer
 * words) is a multi-step sequence over shared I/O ports with NO hardware
 * arbitration.  CLONE_VM threads and the kernel all reach it concurrently
 * (every exec loads an ELF, every file read hits FAT16 -> ata), so without
 * serialisation two transfers interleave their port writes and corrupt each
 * other -- the status poll then spins out its timeout repeatedly and the system
 * stalls.
 *
 * PLAIN spinlock, NOT irqsave: ATA is only called from thread context (vfs ->
 * fat16 -> ata), never an IRQ handler, and a PIO transfer is slow (polled).
 * Disabling interrupts across it would starve the timer and freeze the system.
 * A plain lock keeps the transfer preemptible -- a waiter spins with interrupts
 * enabled, so the timer still fires and reschedules the holder. */
static spinlock_t ata_lock = SPINLOCK_INIT;

static int ata_read_sectors_locked(int drive, uint32_t lba, uint32_t count, void *buf)
{
    if (drive < 0 || drive > 1 || !drive_present[drive])
        return -1;
    if (count == 0)
        return 0;

    uint16_t *ptr = (uint16_t *)buf;

    if (ata_wait_not_busy() < 0)
        return -1;

    /*
     * Send the read command using 28-bit LBA addressing.
     *
     * Drive/Head: 0xE0 = bits [7]=1 [6]=LBA [5]=1 [4]=drive, plus LBA bits 27:24.
     */
    ata_outb(ATA_DRIVE_HEAD,
             (uint8_t)(0xE0 | ((drive & 1) << 4) | ((lba >> 24) & 0x0F)));
    ata_outb(ATA_SECTOR_COUNT, (uint8_t)(count & 0xFF));
    ata_outb(ATA_LBA_LO,  (uint8_t)( lba        & 0xFF));
    ata_outb(ATA_LBA_MID, (uint8_t)((lba >>  8) & 0xFF));
    ata_outb(ATA_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));
    ata_outb(ATA_STATUS,   ATA_CMD_READ_SECTORS);

    /* Read each sector: wait for DRQ, then transfer 256 words (512 bytes) */
    for (uint32_t s = 0; s < count; s++) {
        if (ata_wait_drq() < 0)
            return -1;

        for (int w = 0; w < 256; w++)
            ptr[w] = ata_inw(ATA_DATA);

        ptr += 256;   /* advance buffer pointer by one sector (512 bytes) */
    }

    return 0;
}

int ata_read_sectors(int drive, uint32_t lba, uint32_t count, void *buf)
{
    /* Prefer Bus Master DMA when the controller supports it; fall back to PIO
     * on any DMA error.  Both paths share the same ATA I/O ports (and the DMA
     * path the bounce buffer/PRDT), so the same plain spinlock serialises them.
     * A plain lock is correct here: DMA busy-polls with interrupts enabled so
     * IRQ14 can fire, exactly like the PIO poll loop. */
    spinlock_acquire(&ata_lock);
    int r = -1;
    if (ata_dma_available())
        r = ata_dma_read(drive, lba, count, buf);
    if (r != 0)
        r = ata_read_sectors_locked(drive, lba, count, buf);
    spinlock_release(&ata_lock);
    return r;
}

static int ata_write_sectors_locked(int drive, uint32_t lba, uint32_t count, const void *buf)
{
    if (drive < 0 || drive > 1 || !drive_present[drive])
        return -1;
    if (count == 0)
        return 0;

    const uint16_t *ptr = (const uint16_t *)buf;

    if (ata_wait_not_busy() < 0)
        return -1;

    ata_outb(ATA_DRIVE_HEAD,
             (uint8_t)(0xE0 | ((drive & 1) << 4) | ((lba >> 24) & 0x0F)));
    ata_outb(ATA_SECTOR_COUNT, (uint8_t)(count & 0xFF));
    ata_outb(ATA_LBA_LO,  (uint8_t)( lba        & 0xFF));
    ata_outb(ATA_LBA_MID, (uint8_t)((lba >>  8) & 0xFF));
    ata_outb(ATA_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));
    ata_outb(ATA_STATUS,   ATA_CMD_WRITE_SECTORS);

    /* Write each sector: wait for DRQ, then send 256 words (512 bytes).
     * After the last sector, issue a cache-flush (0xE7) for data integrity. */
    for (uint32_t s = 0; s < count; s++) {
        if (ata_wait_drq() < 0)
            return -1;

        for (int w = 0; w < 256; w++) {
            asm volatile("outw %w0, %w1"
                         :: "a"(ptr[w]), "Nd"((uint16_t)ATA_DATA));
        }
        ptr += 256;
    }

    /* Cache flush: wait for BSY to clear, then flush write cache */
    ata_wait_not_busy();
    ata_outb(ATA_STATUS, 0xE7);   /* FLUSH CACHE command */
    ata_wait_not_busy();

    return 0;
}

int ata_write_sectors(int drive, uint32_t lba, uint32_t count, const void *buf)
{
    spinlock_acquire(&ata_lock);
    int r = -1;
    if (ata_dma_available())
        r = ata_dma_write(drive, lba, count, buf);
    if (r != 0)
        r = ata_write_sectors_locked(drive, lba, count, buf);
    spinlock_release(&ata_lock);
    return r;
}
