#ifndef _KERNEL_ATA_H
#define _KERNEL_ATA_H

#include <stdint.h>

#define ATA_SECTOR_SIZE  512

/* Drive identifiers passed to ata_read_sectors() */
#define ATA_MASTER  0   /* primary bus, master drive */
#define ATA_SLAVE   1   /* primary bus, slave  drive */

/*
 * ata_initialize — probe the primary ATA bus for attached drives.
 *
 * Sends an IDENTIFY command to each drive position (master, slave) and records
 * which drives responded.  Must be called after interrupts are initialized.
 *
 * Returns the number of drives found (0, 1, or 2).
 */
int ata_initialize(void);

/*
 * ata_read_sectors — read `count` 512-byte sectors from `drive` at 28-bit
 *                    LBA address `lba`.  `buf` must be at least
 *                    count * ATA_SECTOR_SIZE bytes.
 *
 * Uses PIO (Programmed I/O) polling — no DMA, no interrupts.
 * Fine for early boot and hobby kernels; not for performance-critical paths.
 *
 * Returns 0 on success, -1 on error or if the drive is not present.
 */
int ata_read_sectors(int drive, uint32_t lba, uint32_t count, void *buf);

/*
 * ata_write_sectors — write `count` 512-byte sectors to `drive` at LBA `lba`.
 *
 * Uses PIO WRITE SECTORS (0x30) command.  `buf` must hold count × 512 bytes.
 * Returns 0 on success, -1 on error or if the drive is not present.
 */
int ata_write_sectors(int drive, uint32_t lba, uint32_t count, const void *buf);

/* Returns 1 if the given drive was detected during ata_initialize(). */
int ata_drive_present(int drive);

#endif /* _KERNEL_ATA_H */
