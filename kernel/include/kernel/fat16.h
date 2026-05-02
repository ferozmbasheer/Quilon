#ifndef _KERNEL_FAT16_H
#define _KERNEL_FAT16_H

#include <stdint.h>
#include <kernel/vfs.h>

/*
 * fat16_ctx_t — state for a mounted FAT16 filesystem.
 *
 * Callers fill in `sector_read` and `ctx` before calling fat16_mount().
 * fat16_mount() parses the BPB from sector 0 and fills in the rest.
 *
 * sector_read(ctx, lba, buf)
 *   Read one 512-byte sector at LBA address `lba` into `buf`.
 *   Returns 0 on success, -1 on error.
 *   The callback is provided by the caller so the FAT16 driver is
 *   independent of the underlying medium: the kernel supplies an ATA
 *   wrapper; unit tests supply a RAM-backed stub.
 *
 * ctx
 *   Opaque pointer forwarded to every sector_read call.
 *   Typically a drive index (ATA_MASTER / ATA_SLAVE) cast to void*.
 */
typedef struct {
    /* ── Caller-supplied I/O back-end ───────────────────────────────────── */
    int   (*sector_read) (void *ctx, uint32_t lba, void *buf);
    int   (*sector_write)(void *ctx, uint32_t lba, const void *buf);  /* NEW */
    void  *ctx;

    /* ── Populated by fat16_mount() from the BPB ────────────────────────── */
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t sectors_per_fat;

    /* Derived LBA start addresses of key regions */
    uint32_t fat_lba;       /* first sector of the FAT                */
    uint32_t root_dir_lba;  /* first sector of the root directory     */
    uint32_t data_lba;      /* first sector of the data region        */
} fat16_ctx_t;

/*
 * fat16_mount — parse the BPB from sector 0 and populate `fs`.
 *
 * Must be called before passing `fs` as the ctx to vfs_mount().
 *
 * Returns 0 on success, -1 if:
 *   - sector_read fails, or
 *   - the 0x55 0xAA boot signature is missing (not a valid FAT volume).
 */
int fat16_mount(fat16_ctx_t *fs);

/*
 * fat16_vfs_ops — VFS driver vtable for FAT16.
 *
 * Usage:
 *   fat16_ctx_t fs = { .sector_read = my_read, .ctx = my_ctx };
 *   if (fat16_mount(&fs) == 0)
 *       vfs_mount(&fat16_vfs_ops, &fs);
 */
extern const vfs_ops_t fat16_vfs_ops;

#endif /* _KERNEL_FAT16_H */
