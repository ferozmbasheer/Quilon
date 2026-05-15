/*
 * Quilon OS -- FAT16 Filesystem Driver
 *
 * FAT16 on-disk layout
 * --------------------
 *
 *   Sector 0:           Boot sector / BIOS Parameter Block (BPB)
 *   Sectors 1 … R-1:   Reserved (R = bpb.reserved_sectors)
 *   Sectors R … :      FAT table(s)  (bpb.num_fats copies, each sectors_per_fat long)
 *   Next region:        Root directory  (bpb.root_entry_count × 32-byte entries)
 *   Remaining:          Data clusters   (cluster 2 is the first usable cluster)
 *
 * Cluster addressing
 * ------------------
 *   A "cluster" is the allocation unit: sectors_per_cluster contiguous sectors.
 *   Cluster numbers in FAT entries and directory entries start at 2.
 *   LBA of cluster N = data_lba + (N - 2) × sectors_per_cluster
 *
 * FAT entry values
 * ----------------
 *   0x0000          free cluster
 *   0x0002–0xFFEF   next cluster in chain
 *   0xFFF8–0xFFFF   end-of-chain marker
 *
 * Testability
 * -----------
 *   All disk I/O is done through the fat16_ctx_t.sector_read callback, so
 *   unit tests can supply a RAM-backed reader without touching hardware.
 *
 * Limitations (intentional for a hobby OS)
 * -----------------------------------------
 *   • Only the root directory is searched (no subdirectory support).
 *   • No write support.
 *   • Long File Name (LFN) entries are silently skipped.
 *   • Filenames are compared case-insensitively (path is uppercased).
 */

#include <stdint.h>
#include <string.h>   /* memcmp, memcpy */
#include <kernel/fat16.h>
#include <kernel/vfs.h>

/* -- On-disk structures (packed to match the FAT specification exactly) --- */

typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];              /* 0x00 Jump instruction (EB xx 90)     */
    char     oem[8];              /* 0x03 OEM name                        */
    uint16_t bytes_per_sector;    /* 0x0B Must be 512 for this driver     */
    uint8_t  sectors_per_cluster; /* 0x0D Cluster size in sectors         */
    uint16_t reserved_sectors;    /* 0x0E Sectors before the first FAT    */
    uint8_t  num_fats;            /* 0x10 Usually 2                       */
    uint16_t root_entry_count;    /* 0x11 Max root dir entries (FAT16)    */
    uint16_t total_sectors_16;    /* 0x13 Total sectors (0 if > 65535)    */
    uint8_t  media_type;          /* 0x15 0xF8 = fixed disk               */
    uint16_t sectors_per_fat;     /* 0x16 Sectors per FAT copy            */
    uint16_t sectors_per_track;   /* 0x18 For CHS geometry (ignored)      */
    uint16_t num_heads;           /* 0x1A For CHS geometry (ignored)      */
    uint32_t hidden_sectors;      /* 0x1C Sectors before this partition   */
    uint32_t total_sectors_32;    /* 0x20 Total sectors if > 65535        */
} fat16_bpb_t;

/*
 * FAT16 directory entry -- 32 bytes per entry.
 *
 * name[8] and ext[3] are space-padded uppercase ASCII (8.3 format).
 * first_cluster_hi is always 0 in FAT16 (non-zero only in FAT32).
 */
typedef struct __attribute__((packed)) {
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t last_access_date;
    uint16_t first_cluster_hi;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster;
    uint32_t file_size;
} fat16_dirent_t;

/* Directory entry attribute flags */
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_LFN        0x0F   /* all 4 low attr bits set = LFN entry */

/* FAT entry end-of-chain range */
#define FAT16_EOC  0xFFF8u

/* -- Forward declarations -------------------------------------------------- */
static uint16_t fat16_next_cluster(fat16_ctx_t *fs, uint16_t cluster);

/* -- Shared sector buffers ------------------------------------------------- */
/*
 * sector_buf:  metadata I/O -- FAT table reads/writes and root-directory
 *              sector reads/writes.
 * data_buf:    data-cluster read-modify-write in fat16_write().
 *
 * Using two buffers prevents fat16_alloc_cluster() (which reads a FAT sector
 * into sector_buf) from clobbering an in-progress data-sector modification.
 * Safe for a single-task, non-preemptive kernel.
 */
static uint8_t sector_buf[512];
static uint8_t data_buf[512];

/* -- Internal helpers ------------------------------------------------------ */

/*
 * fat16_write_fat_entry -- set the FAT entry for `cluster` to `value`.
 *
 * Performs a read-modify-write on the FAT sector containing the entry.
 * Uses sector_buf.  Returns 0 on success, -1 on I/O error.
 */
static int fat16_write_fat_entry(fat16_ctx_t *fs,
                                  uint16_t cluster, uint16_t value)
{
    if (!fs->sector_write) return -1;

    uint32_t fat_offset = (uint32_t)cluster * 2;
    uint32_t fat_sector = fs->fat_lba + fat_offset / 512;
    uint32_t entry_off  = fat_offset % 512;

    if (fs->sector_read(fs->ctx, fat_sector, sector_buf) < 0) return -1;
    sector_buf[entry_off]     = (uint8_t)(value & 0xFF);
    sector_buf[entry_off + 1] = (uint8_t)(value >> 8);
    if (fs->sector_write(fs->ctx, fat_sector, sector_buf) < 0) return -1;
    return 0;
}

/*
 * fat16_alloc_cluster -- find a free FAT entry, mark it end-of-chain (0xFFFF),
 *                       and return the cluster number.
 *
 * Returns a cluster number >= 2 on success, 0 on disk-full or I/O error.
 * Uses sector_buf.
 */
static uint16_t fat16_alloc_cluster(fat16_ctx_t *fs)
{
    if (!fs->sector_write) return 0;

    /* Each 512-byte FAT sector holds 256 two-byte entries. */
    int entries_per_sector = (int)(512 / 2);

    for (uint32_t sec = 0; sec < fs->sectors_per_fat; sec++) {
        if (fs->sector_read(fs->ctx, fs->fat_lba + sec, sector_buf) < 0)
            return 0;

        for (int i = 0; i < entries_per_sector; i++) {
            uint16_t cluster = (uint16_t)(sec * (uint32_t)entries_per_sector + i);
            if (cluster < 2) continue;   /* entries 0 and 1 are reserved */

            uint16_t entry = (uint16_t)( (uint16_t)sector_buf[i * 2]
                                       | ((uint16_t)sector_buf[i * 2 + 1] << 8) );
            if (entry == 0x0000) {
                /* Mark as end-of-chain and write back */
                sector_buf[i * 2]     = 0xFF;
                sector_buf[i * 2 + 1] = 0xFF;
                if (fs->sector_write(fs->ctx, fs->fat_lba + sec, sector_buf) < 0)
                    return 0;
                return cluster;
            }
        }
    }
    return 0;   /* disk full */
}

/*
 * fat16_free_chain -- walk the FAT chain from `first_cluster` and mark every
 *                    entry as free (0x0000).  Used by fat16_remove().
 *
 * Uses sector_buf.  Returns 0 on success, -1 on error.
 */
static int fat16_free_chain(fat16_ctx_t *fs, uint16_t first_cluster)
{
    uint16_t cluster = first_cluster;
    while (cluster >= 2 && cluster < (uint16_t)FAT16_EOC) {
        uint16_t next = fat16_next_cluster(fs, cluster);   /* reads sector_buf */
        if (fat16_write_fat_entry(fs, cluster, 0x0000) < 0)
            return -1;
        cluster = next;
    }
    return 0;
}

/*
 * fat16_update_dirent -- update the directory entry at `dir_sector`[`idx`]
 *                       with the new file size and (optionally) first_cluster.
 *
 * Pass first_cluster = 0 to leave the existing cluster field unchanged.
 * Uses sector_buf.  Returns 0 on success, -1 on error.
 */
static int fat16_update_dirent(fat16_ctx_t *fs,
                                uint32_t dir_sector, uint8_t idx,
                                uint32_t new_size, uint16_t new_first_cluster)
{
    if (!fs->sector_write) return -1;
    if (fs->sector_read(fs->ctx, dir_sector, sector_buf) < 0) return -1;

    fat16_dirent_t *dir = (fat16_dirent_t *)(void *)sector_buf;
    dir[idx].file_size = new_size;
    if (new_first_cluster != 0)
        dir[idx].first_cluster = new_first_cluster;

    if (fs->sector_write(fs->ctx, dir_sector, sector_buf) < 0) return -1;
    return 0;
}

/*
 * fat16_next_cluster -- look up the FAT entry for `cluster`.
 *
 * Returns the next cluster number (2..0xFFF7), FAT16_EOC (>= 0xFFF8) for
 * end-of-chain, or 0 on read error.
 */
static uint16_t fat16_next_cluster(fat16_ctx_t *fs, uint16_t cluster)
{
    uint32_t fat_offset = (uint32_t)cluster * 2;
    uint32_t fat_sector = fs->fat_lba + fat_offset / 512;
    uint32_t entry_off  = fat_offset % 512;

    if (fs->sector_read(fs->ctx, fat_sector, sector_buf) < 0)
        return 0;

    uint16_t next = (uint16_t)( (uint16_t)sector_buf[entry_off] |
                                ((uint16_t)sector_buf[entry_off + 1] << 8) );
    return next;
}

/*
 * fat16_cluster_lba -- first sector LBA for data cluster `cluster` (>= 2).
 */
static uint32_t fat16_cluster_lba(fat16_ctx_t *fs, uint16_t cluster)
{
    return fs->data_lba + (uint32_t)(cluster - 2) * fs->sectors_per_cluster;
}

/*
 * path_to_83 -- convert a path string to a space-padded uppercase 8.3 name.
 *
 * Writes exactly 8 bytes to out_name and 3 bytes to out_ext (no NUL).
 * Returns 0 on success, -1 if the name is empty, too long, or has multiple dots.
 */
static int path_to_83(const char *path, char out_name[8], char out_ext[3])
{
    /* Strip leading '/' */
    if (*path == '/') path++;

    for (int i = 0; i < 8; i++) out_name[i] = ' ';
    for (int i = 0; i < 3; i++) out_ext[i]  = ' ';

    int ni = 0, ei = 0, in_ext = 0;

    for (; *path; path++) {
        char c = *path;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);  /* to uppercase */

        if (c == '.') {
            if (in_ext) return -1;  /* multiple dots not allowed */
            in_ext = 1;
            continue;
        }

        if (!in_ext) {
            if (ni >= 8) return -1;
            out_name[ni++] = c;
        } else {
            if (ei >= 3) return -1;
            out_ext[ei++] = c;
        }
    }

    return (ni > 0) ? 0 : -1;
}

/*
 * is_valid_entry -- return 1 if this directory entry represents a real file
 * or directory that should be visible to the caller.
 */
static int is_valid_entry(const fat16_dirent_t *e)
{
    uint8_t first = (uint8_t)e->name[0];
    if (first == 0x00) return -1;   /* end of directory (signal via -1) */
    if (first == 0xE5) return 0;    /* deleted entry */
    if (e->attr == FAT_ATTR_LFN)    return 0;   /* long file name helper */
    if (e->attr & FAT_ATTR_VOLUME_ID) return 0; /* volume label */
    return 1;
}

/* -- VFS driver functions -------------------------------------------------- */

static int fat16_open(void *ctx, const char *path, vfs_node_t *out)
{
    fat16_ctx_t *fs = (fat16_ctx_t *)ctx;
    char name83[8], ext83[3];

    if (path_to_83(path, name83, ext83) < 0)
        return -1;

    uint32_t root_sectors =
        ((uint32_t)fs->root_entry_count * 32u + 511u) / 512u;

    for (uint32_t sec = 0; sec < root_sectors; sec++) {
        if (fs->sector_read(fs->ctx, fs->root_dir_lba + sec, sector_buf) < 0)
            return -1;

        fat16_dirent_t *dir = (fat16_dirent_t *)(void *)sector_buf;
        int ppe = 512 / (int)sizeof(fat16_dirent_t);

        for (int i = 0; i < ppe; i++) {
            int v = is_valid_entry(&dir[i]);
            if (v < 0) return -1;   /* end of directory */
            if (v == 0) continue;

            if (memcmp(dir[i].name, name83, 8) == 0 &&
                memcmp(dir[i].ext,  ext83,  3) == 0) {
                out->inode         = dir[i].first_cluster;
                out->size          = dir[i].file_size;
                out->type          = (dir[i].attr & FAT_ATTR_DIRECTORY)
                                       ? VFS_TYPE_DIR : VFS_TYPE_FILE;
                out->in_use        = 1;
                out->offset        = 0;
                out->dir_sector    = fs->root_dir_lba + sec;
                out->dir_entry_idx = (uint8_t)i;
                return 0;
            }
        }
    }

    return -1;
}

/*
 * fat16_read -- read `size` bytes from `node` starting at byte `offset`.
 *
 * Navigation:
 *   1. Walk the cluster chain from node->inode to find the cluster that
 *      contains byte `offset`.
 *   2. Read sector by sector within and across clusters until `size`
 *      bytes have been copied.
 *
 * Returns bytes read, or -1 on I/O error.
 */
static int fat16_read(void *ctx, vfs_node_t *node, uint32_t offset,
                      uint32_t size, uint8_t *buf)
{
    fat16_ctx_t *fs = (fat16_ctx_t *)ctx;

    if (size == 0) return 0;

    /* Clamp to file size -- do not read past end of file */
    if (offset >= node->size) return 0;
    if (size > node->size - offset) size = node->size - offset;

    uint32_t cluster_size =
        (uint32_t)fs->sectors_per_cluster * fs->bytes_per_sector;

    /* Walk the cluster chain to the one that holds byte `offset` */
    uint32_t start_cluster_idx = offset / cluster_size;
    uint16_t cluster = (uint16_t)node->inode;

    for (uint32_t i = 0; i < start_cluster_idx; i++) {
        uint16_t next = fat16_next_cluster(fs, cluster);
        if (next < 2 || next >= (uint16_t)FAT16_EOC) return 0;
        cluster = next;
    }

    uint32_t bytes_read = 0;
    uint32_t pos = offset;   /* absolute byte position within the file */

    while (bytes_read < size) {
        if (cluster < 2 || cluster >= (uint16_t)FAT16_EOC) break;

        uint32_t cur_cluster_idx = pos / cluster_size;
        uint32_t byte_in_cluster = pos - cur_cluster_idx * cluster_size;
        uint32_t sec_in_cluster  = byte_in_cluster / fs->bytes_per_sector;
        uint32_t byte_in_sector  = byte_in_cluster % fs->bytes_per_sector;

        uint32_t lba = fat16_cluster_lba(fs, cluster) + sec_in_cluster;
        if (fs->sector_read(fs->ctx, lba, sector_buf) < 0)
            return (bytes_read > 0) ? (int)bytes_read : -1;

        /* Copy as many bytes as fit in the rest of this sector */
        uint32_t avail   = fs->bytes_per_sector - byte_in_sector;
        uint32_t to_copy = size - bytes_read;
        if (to_copy > avail) to_copy = avail;

        memcpy(buf + bytes_read, sector_buf + byte_in_sector, to_copy);

        bytes_read += to_copy;
        pos        += to_copy;

        /* Did we cross a cluster boundary? */
        if (pos / cluster_size != cur_cluster_idx) {
            uint16_t next = fat16_next_cluster(fs, cluster);
            if (next < 2 || next >= (uint16_t)FAT16_EOC) break;
            cluster = next;
        }
    }

    return (int)bytes_read;
}

/*
 * fat16_readdir -- return the `index`-th valid directory entry at `path`.
 *
 * path "/" (or empty) -> lists the FAT16 root directory.
 * path "DIRNAME"      -> finds that entry in root, then lists its cluster.
 *                        If first_cluster == 0 the directory is empty.
 * Returns 0 on success, -1 on end-of-directory or I/O error.
 */
static int fat16_readdir(void *ctx, const char *path, uint32_t index,
                          vfs_dirent_t *out)
{
    fat16_ctx_t *fs = (fat16_ctx_t *)ctx;

    int list_root = (!path || path[0] == '\0' ||
                     (path[0] == '/' && path[1] == '\0'));

    if (!list_root) {
        /* Locate the subdirectory entry in the root to get first_cluster. */
        const char *dname = (path[0] == '/') ? path + 1 : path;
        char name83[8], ext83[3];
        if (path_to_83(dname, name83, ext83) < 0) return -1;

        uint32_t root_sectors =
            ((uint32_t)fs->root_entry_count * 32u + 511u) / 512u;
        uint16_t first_cluster = 0xFFFF;

        for (uint32_t sec = 0; sec < root_sectors; sec++) {
            if (fs->sector_read(fs->ctx, fs->root_dir_lba + sec, sector_buf) < 0)
                return -1;
            fat16_dirent_t *dir = (fat16_dirent_t *)(void *)sector_buf;
            int ppe = 512 / (int)sizeof(fat16_dirent_t);
            for (int i = 0; i < ppe; i++) {
                int v = is_valid_entry(&dir[i]);
                if (v < 0) goto dir_not_found;
                if (v == 0) continue;
                if ((dir[i].attr & FAT_ATTR_DIRECTORY) &&
                    memcmp(dir[i].name, name83, 8) == 0 &&
                    memcmp(dir[i].ext,  ext83,  3) == 0) {
                    first_cluster = dir[i].first_cluster;
                    goto dir_found;
                }
            }
        }
dir_not_found:
        return -1;
dir_found:
        /* Directories created by fat16_mkdir have first_cluster == 0 (empty). */
        if (first_cluster == 0) return -1;
        /* Non-zero cluster chains are not yet traversed -- empty listing. */
        (void)first_cluster;
        return -1;
    }

    /* -- Root directory listing (original behaviour) ---------------------- */
    uint32_t root_sectors =
        ((uint32_t)fs->root_entry_count * 32u + 511u) / 512u;
    uint32_t count = 0;

    for (uint32_t sec = 0; sec < root_sectors; sec++) {
        if (fs->sector_read(fs->ctx, fs->root_dir_lba + sec, sector_buf) < 0)
            return -1;

        fat16_dirent_t *dir = (fat16_dirent_t *)(void *)sector_buf;
        int ppe = 512 / (int)sizeof(fat16_dirent_t);

        for (int i = 0; i < ppe; i++) {
            int v = is_valid_entry(&dir[i]);
            if (v < 0) return -1;   /* end of directory */
            if (v == 0) continue;

            if (count == index) {
                /* Build a human-readable "NAME.EXT" string */
                int oi = 0;
                for (int k = 0; k < 8 && dir[i].name[k] != ' '; k++)
                    out->name[oi++] = dir[i].name[k];
                if (dir[i].ext[0] != ' ') {
                    out->name[oi++] = '.';
                    for (int k = 0; k < 3 && dir[i].ext[k] != ' '; k++)
                        out->name[oi++] = dir[i].ext[k];
                }
                out->name[oi] = '\0';
                out->size = dir[i].file_size;
                out->type = (dir[i].attr & FAT_ATTR_DIRECTORY)
                              ? VFS_TYPE_DIR : VFS_TYPE_FILE;
                return 0;
            }
            count++;
        }
    }

    return -1;
}

/*
 * fat16_write -- write `size` bytes from `buf` into `node` starting at byte
 *               `offset`.
 *
 * Behaviour:
 *   • offset must be <= node->size (no sparse-file gaps).
 *   • Existing clusters are reused; new clusters are allocated and chained
 *     when the write extends past the last cluster.
 *   • For a freshly created file with inode == 0, the first cluster is
 *     allocated here and the directory entry's first_cluster field is updated.
 *   • The directory entry's file_size is updated whenever the write extends
 *     the file.
 *   • Write order: FAT chain first, data second, directory entry last.
 *     On a crash between these steps the old directory entry still points to
 *     valid (old) data.
 *
 * Returns bytes written, 0 if nothing was written, -1 on error.
 * Uses sector_buf for FAT/dir-entry operations and data_buf for data sectors.
 */
static int fat16_write(void *ctx, vfs_node_t *node, uint32_t offset,
                       uint32_t size, const uint8_t *buf)
{
    fat16_ctx_t *fs = (fat16_ctx_t *)ctx;

    if (!fs->sector_write) return -1;
    if (!buf || size == 0) return 0;
    if (offset > node->size) return -1;   /* gaps not supported */

    uint32_t cluster_size =
        (uint32_t)fs->sectors_per_cluster * fs->bytes_per_sector;

    /* -- If the file has no clusters yet, allocate the first one ----------- */
    if (node->inode == 0) {
        uint16_t first = fat16_alloc_cluster(fs);
        if (first == 0) return -1;   /* disk full */

        /* Update the in-memory node and the on-disk directory entry. */
        node->inode = first;
        if (fat16_update_dirent(fs, node->dir_sector,
                                node->dir_entry_idx, 0, first) < 0) {
            fat16_write_fat_entry(fs, first, 0x0000);   /* rollback */
            node->inode = 0;
            return -1;
        }
    }

    /* -- Walk the FAT chain to the cluster containing byte `offset` -------- */
    uint32_t start_cluster_idx = offset / cluster_size;
    uint16_t cluster     = (uint16_t)node->inode;
    uint16_t prev_cluster = 0;

    for (uint32_t i = 0; i < start_cluster_idx; i++) {
        uint16_t next = fat16_next_cluster(fs, cluster);
        if (next < 2 || next >= (uint16_t)FAT16_EOC) {
            /* File ended before reaching `offset` -- allocate and chain. */
            uint16_t nc = fat16_alloc_cluster(fs);
            if (nc == 0) return 0;   /* disk full, wrote nothing */
            if (fat16_write_fat_entry(fs, cluster, nc) < 0) {
                fat16_write_fat_entry(fs, nc, 0x0000);
                return 0;
            }
            cluster = nc;
        } else {
            prev_cluster = cluster;
            cluster = next;
        }
        (void)prev_cluster;
    }

    /* -- Write loop: sector by sector ------------------------------------- */
    uint32_t bytes_written = 0;
    uint32_t pos = offset;

    while (bytes_written < size) {
        /* If we've walked off the end of the chain, extend it. */
        if (cluster < 2 || cluster >= (uint16_t)FAT16_EOC) {
            uint16_t nc = fat16_alloc_cluster(fs);
            if (nc == 0) break;   /* disk full */
            /* chain: the cluster we just consumed -> new cluster */
            if (fat16_write_fat_entry(fs,
                    (uint16_t)(pos > 0 ?
                        fat16_next_cluster(fs, (uint16_t)node->inode) :
                        (uint16_t)node->inode),
                    nc) < 0) {
                fat16_write_fat_entry(fs, nc, 0x0000);
                break;
            }
            cluster = nc;
        }

        uint32_t cur_cluster_idx = pos / cluster_size;
        uint32_t byte_in_cluster = pos - cur_cluster_idx * cluster_size;
        uint32_t sec_in_cluster  = byte_in_cluster / fs->bytes_per_sector;
        uint32_t byte_in_sector  = byte_in_cluster % fs->bytes_per_sector;

        uint32_t lba = fat16_cluster_lba(fs, cluster) + sec_in_cluster;

        /* Read-modify-write: preserve bytes not covered by this write. */
        if (fs->sector_read(fs->ctx, lba, data_buf) < 0) break;

        uint32_t avail   = fs->bytes_per_sector - byte_in_sector;
        uint32_t to_copy = size - bytes_written;
        if (to_copy > avail) to_copy = avail;

        memcpy(data_buf + byte_in_sector, buf + bytes_written, to_copy);

        if (fs->sector_write(fs->ctx, lba, data_buf) < 0) break;

        bytes_written += to_copy;
        pos           += to_copy;

        /* Advance to the next cluster when we cross a cluster boundary. */
        if (pos / cluster_size != cur_cluster_idx) {
            uint16_t next = fat16_next_cluster(fs, cluster);
            cluster = next;   /* may be EOC; the loop guard handles that */
        }
    }

    /* -- Update file size in the directory entry if the file grew ----------- */
    uint32_t new_end = offset + bytes_written;
    if (new_end > node->size) {
        node->size = new_end;
        fat16_update_dirent(fs, node->dir_sector,
                            node->dir_entry_idx, new_end, 0);
    }

    return (int)bytes_written;
}

/*
 * fat16_create -- create a new empty file at `path` in the root directory.
 *
 * Steps:
 *   1. Convert path to 8.3 name.
 *   2. Verify the name does not already exist.
 *   3. Allocate one cluster (the file's initial first_cluster; size = 0).
 *   4. Find a free root directory entry (first byte 0x00 or 0xE5).
 *   5. Write the entry: name, ext, attr = 0x20 (archive), first_cluster, size=0.
 *
 * Returns 0 on success, -1 on error.
 * Uses sector_buf.
 */
static int fat16_create(void *ctx, const char *path)
{
    fat16_ctx_t *fs = (fat16_ctx_t *)ctx;
    if (!fs->sector_write) return -1;

    char name83[8], ext83[3];
    if (path_to_83(path, name83, ext83) < 0) return -1;

    uint32_t root_sectors =
        ((uint32_t)fs->root_entry_count * 32u + 511u) / 512u;

    /* -- Pass 1: check for duplicate name --------------------------------- */
    for (uint32_t sec = 0; sec < root_sectors; sec++) {
        if (fs->sector_read(fs->ctx, fs->root_dir_lba + sec, sector_buf) < 0)
            return -1;

        fat16_dirent_t *dir = (fat16_dirent_t *)(void *)sector_buf;
        int ppe = 512 / (int)sizeof(fat16_dirent_t);

        for (int i = 0; i < ppe; i++) {
            uint8_t first = (uint8_t)dir[i].name[0];
            if (first == 0x00) goto find_free;   /* end of directory */
            if (first == 0xE5) continue;
            if (dir[i].attr == FAT_ATTR_LFN) continue;
            if (dir[i].attr & FAT_ATTR_VOLUME_ID) continue;

            if (memcmp(dir[i].name, name83, 8) == 0 &&
                memcmp(dir[i].ext,  ext83,  3) == 0)
                return -1;   /* already exists */
        }
    }

find_free: ;
    /* -- Allocate the first cluster --------------------------------------- */
    uint16_t first_cluster = fat16_alloc_cluster(fs);
    if (first_cluster == 0) return -1;   /* disk full */

    /* -- Pass 2: find a free directory entry and write it ----------------- */
    for (uint32_t sec = 0; sec < root_sectors; sec++) {
        if (fs->sector_read(fs->ctx, fs->root_dir_lba + sec, sector_buf) < 0)
            goto create_fail;

        fat16_dirent_t *dir = (fat16_dirent_t *)(void *)sector_buf;
        int ppe = 512 / (int)sizeof(fat16_dirent_t);

        for (int i = 0; i < ppe; i++) {
            uint8_t first = (uint8_t)dir[i].name[0];
            if (first != 0x00 && first != 0xE5) continue;

            /* Found a free slot -- fill it in. */
            memset(&dir[i], 0, sizeof(fat16_dirent_t));
            memcpy(dir[i].name, name83, 8);
            memcpy(dir[i].ext,  ext83,  3);
            dir[i].attr          = 0x20;           /* archive */
            dir[i].first_cluster = first_cluster;
            dir[i].file_size     = 0;

            if (fs->sector_write(fs->ctx, fs->root_dir_lba + sec,
                                  sector_buf) < 0)
                goto create_fail;

            return 0;
        }
    }

create_fail:
    /* Rollback the cluster allocation. */
    fat16_write_fat_entry(fs, first_cluster, 0x0000);
    return -1;   /* root directory full or I/O error */
}

/*
 * fat16_remove -- delete the file at `path`.
 *
 * Steps:
 *   1. Find the directory entry.
 *   2. Walk and free the entire FAT cluster chain.
 *   3. Mark the directory entry as deleted (first byte = 0xE5).
 *
 * Returns 0 on success, -1 if the file is not found or I/O fails.
 * Uses sector_buf.
 */
static int fat16_remove(void *ctx, const char *path)
{
    fat16_ctx_t *fs = (fat16_ctx_t *)ctx;
    if (!fs->sector_write) return -1;

    char name83[8], ext83[3];
    if (path_to_83(path, name83, ext83) < 0) return -1;

    uint32_t root_sectors =
        ((uint32_t)fs->root_entry_count * 32u + 511u) / 512u;

    for (uint32_t sec = 0; sec < root_sectors; sec++) {
        if (fs->sector_read(fs->ctx, fs->root_dir_lba + sec, sector_buf) < 0)
            return -1;

        fat16_dirent_t *dir = (fat16_dirent_t *)(void *)sector_buf;
        int ppe = 512 / (int)sizeof(fat16_dirent_t);

        for (int i = 0; i < ppe; i++) {
            uint8_t first = (uint8_t)dir[i].name[0];
            if (first == 0x00) return -1;   /* end of directory */
            if (first == 0xE5) continue;
            if (dir[i].attr == FAT_ATTR_LFN) continue;
            if (dir[i].attr & FAT_ATTR_VOLUME_ID) continue;

            if (memcmp(dir[i].name, name83, 8) != 0 ||
                memcmp(dir[i].ext,  ext83,  3) != 0)
                continue;

            /* Found -- free the cluster chain first, then delete the entry. */
            uint16_t first_cluster = dir[i].first_cluster;
            fat16_free_chain(fs, first_cluster);   /* uses sector_buf */

            /* Re-read the directory sector (free_chain may have clobbered it
             * via sector_buf if the FAT and root dir share no sectors, but
             * reading it again is safe and cheap).                           */
            if (fs->sector_read(fs->ctx, fs->root_dir_lba + sec,
                                  sector_buf) < 0)
                return -1;

            dir = (fat16_dirent_t *)(void *)sector_buf;
            dir[i].name[0] = (char)0xE5;   /* mark deleted */

            if (fs->sector_write(fs->ctx, fs->root_dir_lba + sec,
                                  sector_buf) < 0)
                return -1;

            return 0;
        }
    }

    return -1;   /* not found */
}

/*
 * fat16_stat -- return metadata for the file/directory at `path`.
 *
 * Scans the root directory; "/" is handled as a special case (root dir).
 * Returns 0 and fills `out` on success, -1 if not found.
 */
static int fat16_stat(void *ctx, const char *path, vfs_stat_t *out)
{
    fat16_ctx_t *fs = (fat16_ctx_t *)ctx;

    /* "/" is always the root directory */
    if (path[0] == '/' && path[1] == '\0') {
        out->size = 0;
        out->type = VFS_TYPE_DIR;
        return 0;
    }

    char name83[8], ext83[3];
    if (path_to_83(path, name83, ext83) < 0) return -1;

    uint32_t root_sectors =
        ((uint32_t)fs->root_entry_count * 32u + 511u) / 512u;

    for (uint32_t sec = 0; sec < root_sectors; sec++) {
        if (fs->sector_read(fs->ctx, fs->root_dir_lba + sec, sector_buf) < 0)
            return -1;

        fat16_dirent_t *dir = (fat16_dirent_t *)(void *)sector_buf;
        int ppe = 512 / (int)sizeof(fat16_dirent_t);

        for (int i = 0; i < ppe; i++) {
            int v = is_valid_entry(&dir[i]);
            if (v < 0) return -1;
            if (v == 0) continue;

            if (memcmp(dir[i].name, name83, 8) == 0 &&
                memcmp(dir[i].ext,  ext83,  3) == 0) {
                out->size = dir[i].file_size;
                out->type = (dir[i].attr & FAT_ATTR_DIRECTORY)
                              ? VFS_TYPE_DIR : VFS_TYPE_FILE;
                return 0;
            }
        }
    }
    return -1;
}

/*
 * fat16_mkdir -- create a new empty directory entry in the root directory.
 *
 * Sets attr = FAT_ATTR_DIRECTORY with first_cluster = 0 (empty dir).
 * Subdirectory content ("." and ".." entries) is omitted for simplicity;
 * the dir entry is visible in ls and stat.
 *
 * Returns 0 on success, -1 on duplicate name, disk full, or I/O error.
 */
static int fat16_mkdir(void *ctx, const char *path)
{
    fat16_ctx_t *fs = (fat16_ctx_t *)ctx;
    if (!fs->sector_write) return -1;

    char name83[8], ext83[3];
    if (path_to_83(path, name83, ext83) < 0) return -1;

    uint32_t root_sectors =
        ((uint32_t)fs->root_entry_count * 32u + 511u) / 512u;

    /* Pass 1: check for duplicate name. */
    for (uint32_t sec = 0; sec < root_sectors; sec++) {
        if (fs->sector_read(fs->ctx, fs->root_dir_lba + sec, sector_buf) < 0)
            return -1;

        fat16_dirent_t *dir = (fat16_dirent_t *)(void *)sector_buf;
        int ppe = 512 / (int)sizeof(fat16_dirent_t);

        for (int i = 0; i < ppe; i++) {
            uint8_t first = (uint8_t)dir[i].name[0];
            if (first == 0x00) goto mkdir_find_free;
            if (first == 0xE5) continue;
            if (dir[i].attr == FAT_ATTR_LFN) continue;
            if (dir[i].attr & FAT_ATTR_VOLUME_ID) continue;
            if (memcmp(dir[i].name, name83, 8) == 0 &&
                memcmp(dir[i].ext,  ext83,  3) == 0)
                return -1;   /* already exists */
        }
    }

mkdir_find_free:;
    /* Pass 2: find a free directory slot and write the entry. */
    for (uint32_t sec = 0; sec < root_sectors; sec++) {
        if (fs->sector_read(fs->ctx, fs->root_dir_lba + sec, sector_buf) < 0)
            return -1;

        fat16_dirent_t *dir = (fat16_dirent_t *)(void *)sector_buf;
        int ppe = 512 / (int)sizeof(fat16_dirent_t);

        for (int i = 0; i < ppe; i++) {
            uint8_t first = (uint8_t)dir[i].name[0];
            if (first != 0x00 && first != 0xE5) continue;

            memset(&dir[i], 0, sizeof(fat16_dirent_t));
            memcpy(dir[i].name, name83, 8);
            memcpy(dir[i].ext,  ext83,  3);
            dir[i].attr          = FAT_ATTR_DIRECTORY;
            dir[i].first_cluster = 0;
            dir[i].file_size     = 0;

            if (fs->sector_write(fs->ctx, fs->root_dir_lba + sec,
                                  sector_buf) < 0)
                return -1;

            return 0;
        }
    }
    return -1;   /* root directory full */
}

/*
 * fat16_rename -- rename the file or directory at `oldpath` to `newpath`.
 *
 * Finds the old directory entry and updates the name/ext fields in-place.
 * Returns -1 if the old name is not found, the new name already exists,
 * or an I/O error occurs.
 */
static int fat16_rename(void *ctx, const char *oldpath, const char *newpath)
{
    fat16_ctx_t *fs = (fat16_ctx_t *)ctx;
    if (!fs->sector_write) return -1;

    char old83[8], oldext83[3];
    char new83[8], newext83[3];
    if (path_to_83(oldpath, old83, oldext83) < 0) return -1;
    if (path_to_83(newpath, new83, newext83) < 0) return -1;

    uint32_t root_sectors =
        ((uint32_t)fs->root_entry_count * 32u + 511u) / 512u;

    /* Pass 1: reject if destination already exists (avoids duplicate entries). */
    for (uint32_t sec = 0; sec < root_sectors; sec++) {
        if (fs->sector_read(fs->ctx, fs->root_dir_lba + sec, sector_buf) < 0)
            return -1;
        fat16_dirent_t *dir = (fat16_dirent_t *)(void *)sector_buf;
        int ppe = 512 / (int)sizeof(fat16_dirent_t);
        for (int i = 0; i < ppe; i++) {
            uint8_t first = (uint8_t)dir[i].name[0];
            if (first == 0x00) goto rename_check_done;
            if (first == 0xE5) continue;
            if (dir[i].attr == FAT_ATTR_LFN) continue;
            if (dir[i].attr & FAT_ATTR_VOLUME_ID) continue;
            if (memcmp(dir[i].name, new83,    8) == 0 &&
                memcmp(dir[i].ext,  newext83, 3) == 0)
                return -1;   /* destination already exists */
        }
    }
rename_check_done:

    /* Pass 2: find the old entry and rename it. */
    for (uint32_t sec = 0; sec < root_sectors; sec++) {
        if (fs->sector_read(fs->ctx, fs->root_dir_lba + sec, sector_buf) < 0)
            return -1;

        fat16_dirent_t *dir = (fat16_dirent_t *)(void *)sector_buf;
        int ppe = 512 / (int)sizeof(fat16_dirent_t);
        int found = 0;

        for (int i = 0; i < ppe; i++) {
            uint8_t first = (uint8_t)dir[i].name[0];
            if (first == 0x00) return -1;   /* end of directory */
            if (first == 0xE5) continue;
            if (dir[i].attr == FAT_ATTR_LFN) continue;
            if (dir[i].attr & FAT_ATTR_VOLUME_ID) continue;

            if (memcmp(dir[i].name, old83, 8) == 0 &&
                memcmp(dir[i].ext, oldext83, 3) == 0) {
                memcpy(dir[i].name, new83, 8);
                memcpy(dir[i].ext, newext83, 3);
                found = 1;
                break;
            }
        }

        if (found) {
            if (fs->sector_write(fs->ctx, fs->root_dir_lba + sec,
                                  sector_buf) < 0)
                return -1;
            return 0;
        }
    }
    return -1;   /* not found */
}

static void fat16_close(void *ctx, vfs_node_t *node)
{
    (void)ctx;
    (void)node;
    /* FAT16 has no per-file kernel state to release */
}

/* -- VFS operations table -------------------------------------------------- */

const vfs_ops_t fat16_vfs_ops = {
    .open    = fat16_open,
    .read    = fat16_read,
    .write   = fat16_write,
    .readdir = fat16_readdir,
    .close   = fat16_close,
    .create  = fat16_create,
    .remove  = fat16_remove,
    .stat    = fat16_stat,
    .mkdir   = fat16_mkdir,
    .rename  = fat16_rename,
};

/* -- Public API ------------------------------------------------------------ */

int fat16_mount(fat16_ctx_t *fs)
{
    /* Read the boot sector */
    if (fs->sector_read(fs->ctx, 0, sector_buf) < 0)
        return -1;

    /* Verify the 0x55 0xAA signature at bytes 510–511 */
    if (sector_buf[510] != 0x55 || sector_buf[511] != 0xAA)
        return -1;

    /* Parse the BPB -- it starts at byte 0 of the boot sector */
    fat16_bpb_t *bpb = (fat16_bpb_t *)(void *)sector_buf;

    fs->bytes_per_sector    = bpb->bytes_per_sector;
    fs->sectors_per_cluster = bpb->sectors_per_cluster;
    fs->reserved_sectors    = bpb->reserved_sectors;
    fs->num_fats            = bpb->num_fats;
    fs->root_entry_count    = bpb->root_entry_count;
    fs->sectors_per_fat     = bpb->sectors_per_fat;

    /* Derive key LBA addresses */
    fs->fat_lba      = fs->reserved_sectors;
    fs->root_dir_lba = fs->fat_lba +
                       (uint32_t)fs->num_fats * fs->sectors_per_fat;
    fs->data_lba     = fs->root_dir_lba +
                       ((uint32_t)fs->root_entry_count * 32u + 511u) / 512u;

    return 0;
}
