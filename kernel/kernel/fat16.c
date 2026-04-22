/*
 * Quilon OS — FAT16 Filesystem Driver
 *
 * FAT16 on-disk layout
 * ────────────────────
 *
 *   Sector 0:           Boot sector / BIOS Parameter Block (BPB)
 *   Sectors 1 … R-1:   Reserved (R = bpb.reserved_sectors)
 *   Sectors R … :      FAT table(s)  (bpb.num_fats copies, each sectors_per_fat long)
 *   Next region:        Root directory  (bpb.root_entry_count × 32-byte entries)
 *   Remaining:          Data clusters   (cluster 2 is the first usable cluster)
 *
 * Cluster addressing
 * ──────────────────
 *   A "cluster" is the allocation unit: sectors_per_cluster contiguous sectors.
 *   Cluster numbers in FAT entries and directory entries start at 2.
 *   LBA of cluster N = data_lba + (N - 2) × sectors_per_cluster
 *
 * FAT entry values
 * ────────────────
 *   0x0000          free cluster
 *   0x0002–0xFFEF   next cluster in chain
 *   0xFFF8–0xFFFF   end-of-chain marker
 *
 * Testability
 * ───────────
 *   All disk I/O is done through the fat16_ctx_t.sector_read callback, so
 *   unit tests can supply a RAM-backed reader without touching hardware.
 *
 * Limitations (intentional for a hobby OS)
 * ─────────────────────────────────────────
 *   • Only the root directory is searched (no subdirectory support).
 *   • No write support.
 *   • Long File Name (LFN) entries are silently skipped.
 *   • Filenames are compared case-insensitively (path is uppercased).
 */

#include <stdint.h>
#include <string.h>   /* memcmp, memcpy */
#include <kernel/fat16.h>
#include <kernel/vfs.h>

/* ── On-disk structures (packed to match the FAT specification exactly) ─── */

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
 * FAT16 directory entry — 32 bytes per entry.
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

/* ── Shared sector buffer ────────────────────────────────────────────────── */
/*
 * One 512-byte buffer reused across all I/O.  Safe for a single-task kernel;
 * callers must be careful not to hold a pointer into this buffer across
 * another read call.
 */
static uint8_t sector_buf[512];

/* ── Internal helpers ────────────────────────────────────────────────────── */

/*
 * fat16_next_cluster — look up the FAT entry for `cluster`.
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
 * fat16_cluster_lba — first sector LBA for data cluster `cluster` (>= 2).
 */
static uint32_t fat16_cluster_lba(fat16_ctx_t *fs, uint16_t cluster)
{
    return fs->data_lba + (uint32_t)(cluster - 2) * fs->sectors_per_cluster;
}

/*
 * path_to_83 — convert a path string to a space-padded uppercase 8.3 name.
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
 * is_valid_entry — return 1 if this directory entry represents a real file
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

/* ── VFS driver functions ────────────────────────────────────────────────── */

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
                out->inode  = dir[i].first_cluster;
                out->size   = dir[i].file_size;
                out->type   = (dir[i].attr & FAT_ATTR_DIRECTORY)
                                ? VFS_TYPE_DIR : VFS_TYPE_FILE;
                out->in_use = 1;
                out->offset = 0;
                return 0;
            }
        }
    }

    return -1;
}

/*
 * fat16_read — read `size` bytes from `node` starting at byte `offset`.
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

    /* Clamp to file size — do not read past end of file */
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
 * fat16_readdir — return the `index`-th valid root directory entry.
 *
 * Skips deleted entries, LFN helpers, and volume labels.
 * Returns 0 on success, -1 on end-of-directory or I/O error.
 */
static int fat16_readdir(void *ctx, uint32_t index, vfs_dirent_t *out)
{
    fat16_ctx_t *fs = (fat16_ctx_t *)ctx;

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

static void fat16_close(void *ctx, vfs_node_t *node)
{
    (void)ctx;
    (void)node;
    /* FAT16 has no per-file kernel state to release */
}

/* ── VFS operations table ────────────────────────────────────────────────── */

const vfs_ops_t fat16_vfs_ops = {
    .open    = fat16_open,
    .read    = fat16_read,
    .readdir = fat16_readdir,
    .close   = fat16_close,
};

/* ── Public API ──────────────────────────────────────────────────────────── */

int fat16_mount(fat16_ctx_t *fs)
{
    /* Read the boot sector */
    if (fs->sector_read(fs->ctx, 0, sector_buf) < 0)
        return -1;

    /* Verify the 0x55 0xAA signature at bytes 510–511 */
    if (sector_buf[510] != 0x55 || sector_buf[511] != 0xAA)
        return -1;

    /* Parse the BPB — it starts at byte 0 of the boot sector */
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
