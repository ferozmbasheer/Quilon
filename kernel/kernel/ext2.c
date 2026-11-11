/*
 * Quilon OS -- ext2 Filesystem Driver (read-only)
 * Section 16.2
 *
 * ext2 on-disk layout
 * -------------------
 *
 *   Bytes 0-1023        Boot block (unused by the filesystem)
 *   Byte  1024          Superblock (1024 bytes)
 *   Next block          Block group descriptor table
 *   Then, per group:    block bitmap, inode bitmap, inode table, data blocks
 *
 * Everything is addressed in *blocks* of `1024 << s_log_block_size` bytes.
 * With 1 KiB blocks the superblock is block 1 and s_first_data_block is 1;
 * with larger blocks the superblock shares block 0 with the boot block and
 * s_first_data_block is 0.  The group descriptor table therefore always
 * starts at s_first_data_block + 1.
 *
 * Inodes
 * ------
 * Inode numbers start at 1; the root directory is always inode 2.  Inode N
 * lives in group (N-1)/inodes_per_group at index (N-1)%inodes_per_group of
 * that group's inode table.  Revision 1 filesystems may use inodes larger
 * than 128 bytes (s_inode_size); only the leading 128 bytes are read.
 *
 * File contents are found through the inode's 15 block pointers: 12 direct,
 * then singly, doubly and triply indirect.  This driver implements direct,
 * singly and doubly indirect -- with 1 KiB blocks that covers files up to
 * (12 + 256 + 256*256) KiB, about 64 MiB, which is far more than Quilon
 * needs.  A zero pointer is a hole and reads as zeroes.
 *
 * Directories
 * -----------
 * A directory's data blocks hold variable-length entries: an 8-byte header
 * (inode, rec_len, name_len, file_type) followed by the name, which is NOT
 * NUL-terminated.  rec_len is the distance to the next entry and always
 * lands the last entry exactly on the end of the block, so iteration walks
 * by rec_len and stops at the block boundary.  An entry with inode == 0 is
 * free space and is skipped.
 *
 * Testability
 * -----------
 * All disk I/O goes through the ext2_ctx_t.sector_read callback, so the host
 * unit tests (tests/test_ext2.c) drive this file against a RAM-backed image
 * with no hardware involved.
 *
 * Limitations (intentional for a hobby OS)
 * ----------------------------------------
 *   - Read-only: no create/write/mkdir/rename/remove.
 *   - No triply indirect blocks, no symlinks, no permissions enforcement.
 *   - Block size must be 1 KiB, 2 KiB or 4 KiB.
 */

#include <stdint.h>
#include <string.h>   /* memcpy, memcmp, memset */
#include <kernel/ext2.h>
#include <kernel/vfs.h>

/* -- Superblock field offsets ---------------------------------------------
 *
 * The superblock is read into a byte buffer and fields are pulled out by
 * offset rather than being overlaid with a struct: the buffer is only
 * 4-byte aligned in general, and this keeps the driver honest about which
 * fields it actually depends on.
 */
#define SB_INODES_COUNT      0
#define SB_BLOCKS_COUNT      4
#define SB_FIRST_DATA_BLOCK  20
#define SB_LOG_BLOCK_SIZE    24
#define SB_BLOCKS_PER_GROUP  32
#define SB_INODES_PER_GROUP  40
#define SB_MAGIC             56
#define SB_REV_LEVEL         76
#define SB_INODE_SIZE        88
#define SB_FEATURE_INCOMPAT  96

/* Block group descriptor: 32 bytes, we only need the inode table pointer. */
#define BGD_SIZE             32
#define BGD_INODE_TABLE      8

/* -- Shared block buffers --------------------------------------------------
 *
 * Three buffers, deliberately separate so a nested call cannot clobber a
 * caller's in-progress block:
 *
 *   meta_buf  superblock, group descriptors, inode table  (ext2_read_inode)
 *   ind_buf   indirect block pointers                     (ext2_bmap)
 *   data_buf  file/directory data                         (ext2_read_data,
 *                                                          ext2_dir_find)
 *
 * ext2_read_data calls ext2_bmap while holding data_buf, and ext2_dir_find
 * calls ext2_read_inode while holding data_buf -- both are safe because the
 * callees use different buffers.  Safe for a single-task, non-preemptive
 * filesystem path, exactly like the FAT16 driver's sector buffers.
 */
static uint8_t meta_buf[EXT2_MAX_BLOCK_SIZE];
static uint8_t ind_buf[EXT2_MAX_BLOCK_SIZE];
static uint8_t data_buf[EXT2_MAX_BLOCK_SIZE];

/* -- Little-endian field readers ------------------------------------------
 *
 * ext2 is little-endian on disk.  i386 is too, but reading through these
 * helpers keeps the code free of unaligned struct casts.
 */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0]        | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* -- Block I/O ------------------------------------------------------------- */

/*
 * ext2_read_block -- read filesystem block `block` into `buf`.
 *
 * A block is sectors_per_block consecutive 512-byte sectors starting at
 * LBA block * sectors_per_block.  Returns 0 on success, -1 on I/O error.
 */
static int ext2_read_block(ext2_ctx_t *fs, uint32_t block, uint8_t *buf)
{
    if (!fs->sector_read) return -1;
    if (block >= fs->blocks_count) return -1;

    uint32_t lba = block * fs->sectors_per_block;
    for (uint32_t i = 0; i < fs->sectors_per_block; i++) {
        if (fs->sector_read(fs->ctx, lba + i, buf + i * 512u) < 0)
            return -1;
    }
    return 0;
}

/* -- Mount ----------------------------------------------------------------- */

/*
 * read_superblock -- load the 1024-byte superblock into `meta_buf`.
 *
 * The superblock always lives at byte offset 1024, i.e. sectors 2 and 3,
 * independent of the filesystem's block size -- so this reads sectors
 * directly rather than going through ext2_read_block (which needs the
 * geometry this function is about to discover).
 */
static int read_superblock(ext2_ctx_t *fs)
{
    if (!fs->sector_read) return -1;
    uint32_t lba = EXT2_SUPERBLOCK_OFF / 512u;   /* = 2 */
    for (uint32_t i = 0; i < 2; i++) {
        if (fs->sector_read(fs->ctx, lba + i, meta_buf + i * 512u) < 0)
            return -1;
    }
    return 0;
}

int ext2_probe(ext2_ctx_t *fs)
{
    if (!fs || read_superblock(fs) < 0) return 0;
    return (rd16(meta_buf + SB_MAGIC) == EXT2_SUPER_MAGIC) ? 1 : 0;
}

int ext2_mount(ext2_ctx_t *fs)
{
    if (!fs) return -1;
    if (read_superblock(fs) < 0) return -1;
    if (rd16(meta_buf + SB_MAGIC) != EXT2_SUPER_MAGIC) return -1;

    uint32_t block_size = ext2_block_size_from_log(rd32(meta_buf + SB_LOG_BLOCK_SIZE));
    if (block_size == 0) return -1;               /* block size > 4 KiB */

    fs->block_size        = block_size;
    fs->sectors_per_block = block_size / 512u;
    fs->inodes_count      = rd32(meta_buf + SB_INODES_COUNT);
    fs->blocks_count      = rd32(meta_buf + SB_BLOCKS_COUNT);
    fs->first_data_block  = rd32(meta_buf + SB_FIRST_DATA_BLOCK);
    fs->blocks_per_group  = rd32(meta_buf + SB_BLOCKS_PER_GROUP);
    fs->inodes_per_group  = rd32(meta_buf + SB_INODES_PER_GROUP);
    fs->rev_level         = rd32(meta_buf + SB_REV_LEVEL);

    /* Revision 0 predates s_inode_size and always uses 128-byte inodes. */
    if (fs->rev_level >= 1) {
        fs->inode_size       = rd16(meta_buf + SB_INODE_SIZE);
        fs->feature_incompat = rd32(meta_buf + SB_FEATURE_INCOMPAT);
    } else {
        fs->inode_size       = EXT2_GOOD_OLD_INODE_SIZE;
        fs->feature_incompat = 0;
    }

    /* An inode must not straddle a block boundary, which the format
     * guarantees by making inode_size a power-of-two divisor of the block
     * size.  Reject anything else rather than mis-read the inode table. */
    if (fs->inode_size < EXT2_GOOD_OLD_INODE_SIZE ||
        fs->inode_size > fs->block_size ||
        (fs->block_size % fs->inode_size) != 0)
        return -1;

    if (fs->inodes_per_group == 0 || fs->blocks_per_group == 0) return -1;

    fs->group_count = ext2_group_count(fs->blocks_count, fs->first_data_block,
                                       fs->blocks_per_group);
    if (fs->group_count == 0) return -1;

    /* The group descriptor table always follows the superblock's block. */
    fs->bgd_block = fs->first_data_block + 1;

    return 0;
}

/* -- Inodes ---------------------------------------------------------------- */

int ext2_read_inode(ext2_ctx_t *fs, uint32_t ino, ext2_inode_t *out)
{
    if (!fs || !out || ino == 0 || ino > fs->inodes_count) return -1;

    uint32_t group = ext2_inode_group(ino, fs->inodes_per_group);
    if (group >= fs->group_count) return -1;

    /* Locate this group's descriptor: BGD_SIZE bytes each, packed from
     * fs->bgd_block onwards (the table may span several blocks). */
    uint32_t bgd_off   = group * BGD_SIZE;
    uint32_t bgd_block = fs->bgd_block + bgd_off / fs->block_size;
    uint32_t bgd_in    = bgd_off % fs->block_size;

    if (ext2_read_block(fs, bgd_block, meta_buf) < 0) return -1;
    uint32_t inode_table = rd32(meta_buf + bgd_in + BGD_INODE_TABLE);

    /* Now the inode itself within that group's inode table. */
    uint32_t index      = ext2_inode_index(ino, fs->inodes_per_group);
    uint32_t byte_off   = index * fs->inode_size;
    uint32_t ino_block  = inode_table + byte_off / fs->block_size;
    uint32_t ino_in     = byte_off % fs->block_size;

    if (ext2_read_block(fs, ino_block, meta_buf) < 0) return -1;
    memcpy(out, meta_buf + ino_in, sizeof(ext2_inode_t));
    return 0;
}

/* -- Block mapping --------------------------------------------------------- */

int ext2_bmap(ext2_ctx_t *fs, const ext2_inode_t *inode,
              uint32_t block_index, uint32_t *out_block)
{
    if (!fs || !inode || !out_block) return -1;

    uint32_t per_block = fs->block_size / 4u;   /* pointers per indirect block */

    /* -- Direct -- */
    if (block_index < EXT2_NDIR_BLOCKS) {
        *out_block = inode->i_block[block_index];
        return 0;
    }
    block_index -= EXT2_NDIR_BLOCKS;

    /* -- Singly indirect -- */
    if (block_index < per_block) {
        uint32_t ind = inode->i_block[EXT2_IND_BLOCK];
        if (ind == 0) { *out_block = 0; return 0; }        /* hole */
        if (ext2_read_block(fs, ind, ind_buf) < 0) return -1;
        *out_block = rd32(ind_buf + block_index * 4u);
        return 0;
    }
    block_index -= per_block;

    /* -- Doubly indirect -- */
    if (block_index < per_block * per_block) {
        uint32_t dind = inode->i_block[EXT2_DIND_BLOCK];
        if (dind == 0) { *out_block = 0; return 0; }
        if (ext2_read_block(fs, dind, ind_buf) < 0) return -1;

        uint32_t mid = rd32(ind_buf + (block_index / per_block) * 4u);
        if (mid == 0) { *out_block = 0; return 0; }
        if (ext2_read_block(fs, mid, ind_buf) < 0) return -1;

        *out_block = rd32(ind_buf + (block_index % per_block) * 4u);
        return 0;
    }

    /* Triply indirect is not supported -- see the file header. */
    return -1;
}

/* -- File data ------------------------------------------------------------- */

int ext2_read_data(ext2_ctx_t *fs, const ext2_inode_t *inode,
                   uint32_t offset, uint32_t size, uint8_t *buf)
{
    if (!fs || !inode || !buf) return -1;
    if (size == 0) return 0;

    /* Clamp to the file size -- never read past EOF. */
    if (offset >= inode->i_size) return 0;
    if (size > inode->i_size - offset) size = inode->i_size - offset;

    uint32_t done = 0;
    while (done < size) {
        uint32_t pos          = offset + done;
        uint32_t block_index  = pos / fs->block_size;
        uint32_t byte_in_blk  = pos % fs->block_size;
        uint32_t avail        = fs->block_size - byte_in_blk;
        uint32_t to_copy      = size - done;
        if (to_copy > avail) to_copy = avail;

        uint32_t block;
        if (ext2_bmap(fs, inode, block_index, &block) < 0)
            return (done > 0) ? (int)done : -1;

        if (block == 0) {
            /* Sparse file hole: reads as zeroes. */
            memset(buf + done, 0, to_copy);
        } else {
            if (ext2_read_block(fs, block, data_buf) < 0)
                return (done > 0) ? (int)done : -1;
            memcpy(buf + done, data_buf + byte_in_blk, to_copy);
        }
        done += to_copy;
    }
    return (int)done;
}

/* -- Directories ----------------------------------------------------------- */

/*
 * name_matches -- compare a path component against an on-disk entry name.
 *
 * On-disk names are not NUL-terminated, so the length must match exactly as
 * well as the bytes.  ext2 names are case-sensitive, so this is an exact
 * byte comparison -- see ci_name_matches for the fallback.
 */
static int name_matches(const uint8_t *disk_name, uint8_t name_len,
                        const char *want, uint32_t want_len)
{
    if (name_len != want_len) return 0;
    return memcmp(disk_name, want, name_len) == 0;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/*
 * ci_name_matches -- case-insensitive comparison, used only as a fallback.
 *
 * Why a hobby OS needs this: Quilon's keyboard driver has a single unshifted
 * scancode map, so no uppercase letter can be typed at the shell -- yet the
 * disk image stores uppercase names (SHELL.ELF and friends, matching the
 * FAT16 convention the kernel and both shells already hard-code).  Without a
 * fallback, "cat readme.txt" could never be typed and ext2 volumes would be
 * unusable interactively.
 *
 * ext2 itself is case-sensitive, so an exact match always wins: ext2_dir_find
 * scans the whole directory for one first, and only then accepts a
 * case-insensitive hit.  A directory holding both "Foo" and "foo" therefore
 * still resolves each name correctly.
 */
static int ci_name_matches(const uint8_t *disk_name, uint8_t name_len,
                           const char *want, uint32_t want_len)
{
    if (name_len != want_len) return 0;
    for (uint32_t i = 0; i < want_len; i++) {
        if (lower((char)disk_name[i]) != lower(want[i])) return 0;
    }
    return 1;
}

/*
 * ext2_dir_find -- find the entry named `name` (length `name_len`) in the
 * directory inode `dir`, returning its inode number in `*out_ino`.
 *
 * Walks every data block of the directory, iterating entries by rec_len.
 * An exact (case-sensitive) match wins; a case-insensitive match is only
 * used if the whole directory holds no exact one -- see ci_name_matches.
 *
 * Returns 0 on success, -1 if not found or on I/O error.  Uses data_buf.
 */
static int ext2_dir_find(ext2_ctx_t *fs, const ext2_inode_t *dir,
                         const char *name, uint32_t name_len,
                         uint32_t *out_ino)
{
    if ((dir->i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;
    if (name_len == 0 || name_len > EXT2_NAME_LEN)   return -1;

    uint32_t nblocks = (dir->i_size + fs->block_size - 1) / fs->block_size;
    uint32_t ci_ino  = 0;   /* best case-insensitive match so far */

    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t block;
        if (ext2_bmap(fs, dir, bi, &block) < 0) return -1;
        if (block == 0) continue;                      /* hole */
        if (ext2_read_block(fs, block, data_buf) < 0) return -1;

        uint32_t off = 0;
        while (off + sizeof(ext2_dirent_t) <= fs->block_size) {
            uint32_t e_ino    = rd32(data_buf + off);
            uint16_t rec_len  = rd16(data_buf + off + 4);
            uint8_t  n_len    = data_buf[off + 6];

            /* A corrupt rec_len would loop forever or walk off the block. */
            if (rec_len < ext2_dirent_min_len(n_len) ||
                off + rec_len > fs->block_size)
                break;

            if (e_ino != 0) {
                if (name_matches(data_buf + off + 8, n_len, name, name_len)) {
                    *out_ino = e_ino;
                    return 0;
                }
                if (ci_ino == 0 &&
                    ci_name_matches(data_buf + off + 8, n_len, name, name_len))
                    ci_ino = e_ino;
            }
            off += rec_len;
        }
    }

    if (ci_ino != 0) { *out_ino = ci_ino; return 0; }
    return -1;
}

/* -- Path resolution ------------------------------------------------------- */

int ext2_lookup_path(ext2_ctx_t *fs, const char *path,
                     uint32_t *out_ino, ext2_inode_t *out_inode)
{
    if (!fs || !path || !out_ino) return -1;

    uint32_t     ino = EXT2_ROOT_INO;
    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) < 0) return -1;

    const char *p = path;
    while (*p == '/') p++;                 /* absolute and relative alike */

    while (*p) {
        /* Measure the next component. */
        const char *start = p;
        while (*p && *p != '/') p++;
        uint32_t len = (uint32_t)(p - start);
        while (*p == '/') p++;             /* skip separators, tolerate "//" */

        if (len == 0) continue;            /* trailing slash */

        /* Every component except the last must be a directory. */
        if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

        uint32_t next;
        if (ext2_dir_find(fs, &inode, start, len, &next) < 0) return -1;
        if (ext2_read_inode(fs, next, &inode) < 0) return -1;
        ino = next;
    }

    *out_ino = ino;
    if (out_inode) *out_inode = inode;
    return 0;
}

/* -- VFS driver functions -------------------------------------------------- */

/* Map an ext2 i_mode to a VFS_TYPE_* value. */
static uint8_t vfs_type_of(uint16_t i_mode)
{
    return ((i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? VFS_TYPE_DIR
                                                    : VFS_TYPE_FILE;
}

static int ext2_vfs_open(void *ctx, const char *path, vfs_node_t *out)
{
    ext2_ctx_t  *fs = (ext2_ctx_t *)ctx;
    uint32_t     ino;
    ext2_inode_t inode;

    if (!out) return -1;
    if (ext2_lookup_path(fs, path, &ino, &inode) < 0) return -1;

    /* node->inode holds the ext2 inode *number*; the inode itself is re-read
     * on each operation, so an open fd never goes stale.  dir_sector and
     * dir_entry_idx exist for FAT16's in-place directory updates and stay 0
     * for this read-only driver. */
    out->inode         = ino;
    out->size          = inode.i_size;
    out->type          = vfs_type_of(inode.i_mode);
    out->in_use        = 1;
    out->offset        = 0;
    out->dir_sector    = 0;
    out->dir_entry_idx = 0;
    return 0;
}

static int ext2_vfs_read(void *ctx, vfs_node_t *node, uint32_t offset,
                         uint32_t size, uint8_t *buf)
{
    ext2_ctx_t  *fs = (ext2_ctx_t *)ctx;
    ext2_inode_t inode;

    if (!node || !buf) return -1;
    if (ext2_read_inode(fs, node->inode, &inode) < 0) return -1;
    return ext2_read_data(fs, &inode, offset, size, buf);
}

static int ext2_vfs_readdir(void *ctx, const char *path, uint32_t index,
                            vfs_dirent_t *out)
{
    ext2_ctx_t  *fs = (ext2_ctx_t *)ctx;
    uint32_t     dir_ino;
    ext2_inode_t dir;

    if (!out) return -1;
    if (!path || path[0] == '\0') path = "/";
    if (ext2_lookup_path(fs, path, &dir_ino, &dir) < 0) return -1;
    if ((dir.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    uint32_t nblocks = (dir.i_size + fs->block_size - 1) / fs->block_size;
    uint32_t seen    = 0;

    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t block;
        if (ext2_bmap(fs, &dir, bi, &block) < 0) return -1;
        if (block == 0) continue;
        if (ext2_read_block(fs, block, data_buf) < 0) return -1;

        uint32_t off = 0;
        while (off + sizeof(ext2_dirent_t) <= fs->block_size) {
            uint32_t e_ino   = rd32(data_buf + off);
            uint16_t rec_len = rd16(data_buf + off + 4);
            uint8_t  n_len   = data_buf[off + 6];

            if (rec_len < ext2_dirent_min_len(n_len) ||
                off + rec_len > fs->block_size)
                break;

            if (e_ino != 0) {
                if (seen == index) {
                    /* Names longer than the VFS limit are truncated rather
                     * than skipped, so `ls` still shows the file. */
                    uint32_t copy = n_len;
                    if (copy > VFS_NAME_MAX) copy = VFS_NAME_MAX;
                    memcpy(out->name, data_buf + off + 8, copy);
                    out->name[copy] = '\0';

                    /* Size and type come from the inode: the on-disk
                     * file_type byte is only valid when the FILETYPE feature
                     * is set, and the size is not in the directory entry. */
                    ext2_inode_t ent;
                    if (ext2_read_inode(fs, e_ino, &ent) < 0) return -1;
                    out->size = ent.i_size;
                    out->type = vfs_type_of(ent.i_mode);
                    return 0;
                }
                seen++;
            }
            off += rec_len;
        }
    }
    return -1;   /* end of directory */
}

static void ext2_vfs_close(void *ctx, vfs_node_t *node)
{
    (void)ctx; (void)node;   /* nothing to release: no per-open state */
}

static int ext2_vfs_stat(void *ctx, const char *path, vfs_stat_t *out)
{
    ext2_ctx_t  *fs = (ext2_ctx_t *)ctx;
    uint32_t     ino;
    ext2_inode_t inode;

    if (!out) return -1;
    if (ext2_lookup_path(fs, path, &ino, &inode) < 0) return -1;

    out->type = vfs_type_of(inode.i_mode);
    out->size = (out->type == VFS_TYPE_DIR) ? 0 : inode.i_size;
    return 0;
}

static int ext2_vfs_fsinfo(void *ctx, vfs_fsinfo_t *out)
{
    ext2_ctx_t *fs = (ext2_ctx_t *)ctx;
    if (!fs || !out) return -1;

    const char *name = "ext2";
    uint32_t    i    = 0;
    while (name[i] && i < VFS_FSNAME_MAX - 1) { out->type[i] = name[i]; i++; }
    out->type[i] = '\0';

    out->block_size   = fs->block_size;
    out->total_blocks = fs->blocks_count;
    out->name_max     = VFS_NAME_MAX;
    return 0;
}

/*
 * Read-only: write, create, remove, mkdir and rename are left NULL so the
 * VFS layer reports them as unsupported instead of silently doing nothing.
 */
const vfs_ops_t ext2_vfs_ops = {
    .open    = ext2_vfs_open,
    .read    = ext2_vfs_read,
    .write   = (int (*)(void *, vfs_node_t *, uint32_t, uint32_t,
                        const uint8_t *))0,
    .readdir = ext2_vfs_readdir,
    .close   = ext2_vfs_close,
    .create  = (int (*)(void *, const char *))0,
    .remove  = (int (*)(void *, const char *))0,
    .stat    = ext2_vfs_stat,
    .mkdir   = (int (*)(void *, const char *))0,
    .rename  = (int (*)(void *, const char *, const char *))0,
    .fsinfo  = ext2_vfs_fsinfo,
};
