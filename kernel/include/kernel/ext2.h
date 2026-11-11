#ifndef _KERNEL_EXT2_H
#define _KERNEL_EXT2_H

#include <stdint.h>
#include <kernel/vfs.h>

/*
 * Quilon OS -- ext2 filesystem driver (section 16.2)
 *
 * Read-only ext2 (revision 0 and 1).  Unlike the FAT16 driver this one does
 * real path traversal: "/bin/hello.elf" walks the directory tree from the
 * root inode instead of only scanning a flat root directory.
 *
 * See ext2.c for the on-disk layout notes.
 */

/* Largest block size the driver supports.  ext2 allows 1 KiB, 2 KiB and
 * 4 KiB; anything larger is rejected at mount time.  This bounds the static
 * block buffers in ext2.c. */
#define EXT2_MAX_BLOCK_SIZE  4096

#define EXT2_SUPER_MAGIC     0xEF53
#define EXT2_SUPERBLOCK_OFF  1024   /* byte offset of the superblock       */
#define EXT2_ROOT_INO        2      /* inode number of the root directory  */
#define EXT2_GOOD_OLD_INODE_SIZE 128
#define EXT2_NDIR_BLOCKS     12     /* direct block pointers in an inode   */
#define EXT2_IND_BLOCK       12     /* index of the singly indirect ptr    */
#define EXT2_DIND_BLOCK      13     /* index of the doubly indirect ptr    */
#define EXT2_TIND_BLOCK      14     /* index of the triply indirect ptr    */
#define EXT2_NAME_LEN        255

/* i_mode format bits (the high nibble) */
#define EXT2_S_IFMT          0xF000
#define EXT2_S_IFREG         0x8000
#define EXT2_S_IFDIR         0x4000

/* Incompatible feature flags we care about */
#define EXT2_FEATURE_INCOMPAT_FILETYPE  0x0002

/* -- On-disk structures ---------------------------------------------------- */

/*
 * ext2_inode_t -- the first 128 bytes of an on-disk inode.
 *
 * Revision 1 filesystems may use a larger inode (s_inode_size, typically
 * 256); the extra bytes hold timestamps and extended attributes we ignore,
 * so the driver always copies just this leading 128-byte part.
 */
typedef struct __attribute__((packed)) {
    uint16_t i_mode;          /* file type + permissions                   */
    uint16_t i_uid;
    uint32_t i_size;          /* size in bytes (low 32 bits)               */
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;        /* size in 512-byte units, not fs blocks     */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];     /* 12 direct, 1 single, 1 double, 1 triple   */
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;       /* high 32 bits of size for large files      */
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} ext2_inode_t;

/*
 * ext2_dirent_t -- header of a variable-length directory entry.
 *
 * The name follows immediately after this 8-byte header and is NOT
 * NUL-terminated; `name_len` gives its length.  `rec_len` is the distance to
 * the next entry, so entries are walked by adding rec_len (never by
 * sizeof(name)).  `file_type` is only meaningful when the FILETYPE
 * incompatible feature is set; the driver reads the inode instead, so it
 * works either way.
 */
typedef struct __attribute__((packed)) {
    uint32_t inode;      /* 0 = unused entry (skip it)          */
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
} ext2_dirent_t;

/* -- Mounted filesystem context -------------------------------------------- */

/*
 * ext2_ctx_t -- state for a mounted ext2 filesystem.
 *
 * Callers fill in `sector_read` (and optionally `sector_write`) plus `ctx`
 * before calling ext2_mount(), exactly like fat16_ctx_t.  Everything below
 * the separator is filled in by ext2_mount() from the superblock.
 *
 * sector_read(ctx, lba, buf)
 *   Read one 512-byte sector at LBA `lba` into `buf`.  0 on success, -1 on
 *   error.  Supplied by the caller so the driver is medium-independent: the
 *   kernel passes an ATA wrapper, unit tests pass a RAM-backed stub.
 */
typedef struct {
    /* -- Caller-supplied I/O back-end ------------------------------------- */
    int   (*sector_read) (void *ctx, uint32_t lba, void *buf);
    int   (*sector_write)(void *ctx, uint32_t lba, const void *buf);
    void  *ctx;

    /* -- Populated by ext2_mount() from the superblock -------------------- */
    uint32_t block_size;        /* 1024 << s_log_block_size                 */
    uint32_t sectors_per_block; /* block_size / 512                         */
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t first_data_block;  /* 1 when block_size == 1024, else 0        */
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;        /* 128 on rev 0, s_inode_size on rev 1      */
    uint32_t rev_level;
    uint32_t feature_incompat;
    uint32_t group_count;       /* number of block groups                   */
    uint32_t bgd_block;         /* block holding the group descriptor table */
} ext2_ctx_t;

/* -- Pure helpers (host-testable, no I/O) ---------------------------------- */

/*
 * ext2_block_size_from_log -- decode s_log_block_size.
 *
 * Returns 0 for a value the driver refuses to mount (block size above
 * EXT2_MAX_BLOCK_SIZE), so callers can treat 0 as "unsupported".
 */
static inline uint32_t ext2_block_size_from_log(uint32_t log_block_size)
{
    if (log_block_size > 2) return 0;            /* > 4096 unsupported */
    return 1024u << log_block_size;
}

/* Block group containing inode `ino` (inode numbers start at 1). */
static inline uint32_t ext2_inode_group(uint32_t ino, uint32_t inodes_per_group)
{
    if (ino == 0 || inodes_per_group == 0) return 0;
    return (ino - 1) / inodes_per_group;
}

/* Index of inode `ino` within its group's inode table. */
static inline uint32_t ext2_inode_index(uint32_t ino, uint32_t inodes_per_group)
{
    if (ino == 0 || inodes_per_group == 0) return 0;
    return (ino - 1) % inodes_per_group;
}

/*
 * ext2_group_count -- number of block groups in the filesystem.
 *
 * Blocks before s_first_data_block (the boot block on a 1 KiB filesystem)
 * are not part of any group, hence the subtraction.
 */
static inline uint32_t ext2_group_count(uint32_t blocks_count,
                                        uint32_t first_data_block,
                                        uint32_t blocks_per_group)
{
    if (blocks_per_group == 0 || blocks_count <= first_data_block) return 0;
    uint32_t data_blocks = blocks_count - first_data_block;
    return (data_blocks + blocks_per_group - 1) / blocks_per_group;
}

/*
 * ext2_dirent_min_len -- minimum rec_len for an entry with `name_len` bytes
 * of name: the 8-byte header plus the name, rounded up to a multiple of 4.
 *
 * Used to sanity-check rec_len while walking a directory block so a corrupt
 * (or zero) rec_len cannot spin the iteration forever.
 */
static inline uint16_t ext2_dirent_min_len(uint8_t name_len)
{
    return (uint16_t)((8u + name_len + 3u) & ~3u);
}

/* -- Driver API ------------------------------------------------------------ */

/*
 * ext2_mount -- read and validate the superblock, fill in `fs`.
 *
 * Returns 0 on success, -1 if sector_read fails, the magic is not 0xEF53,
 * or the filesystem uses a geometry the driver does not support (block size
 * above 4 KiB, or an inode size that is not a power-of-two divisor of the
 * block size).
 */
int ext2_mount(ext2_ctx_t *fs);

/*
 * ext2_probe -- return 1 if the volume behind `sector_read`/`ctx` carries an
 * ext2 superblock, 0 otherwise.  Only `sector_read` and `ctx` need be set.
 *
 * Used at boot to pick between the ext2 and FAT16 drivers.
 */
int ext2_probe(ext2_ctx_t *fs);

/*
 * ext2_read_inode -- read inode number `ino` into `out`.
 *
 * Returns 0 on success, -1 on I/O error or an out-of-range inode number.
 */
int ext2_read_inode(ext2_ctx_t *fs, uint32_t ino, ext2_inode_t *out);

/*
 * ext2_bmap -- map a file-relative block index to an absolute block number.
 *
 * Follows the direct, singly indirect and doubly indirect pointers.  Triply
 * indirect blocks are not supported (files above ~64 MiB with 1 KiB blocks).
 * Writes the block number to `*out_block` -- 0 means a hole (or beyond the
 * end of the file), which callers read as zeroes.
 *
 * Returns 0 on success, -1 on I/O error or an unsupported block index.
 */
int ext2_bmap(ext2_ctx_t *fs, const ext2_inode_t *inode,
              uint32_t block_index, uint32_t *out_block);

/*
 * ext2_read_data -- read `size` bytes at byte `offset` from `inode`.
 *
 * Clamps to the inode's size.  Returns bytes read (0 at EOF), -1 on error.
 */
int ext2_read_data(ext2_ctx_t *fs, const ext2_inode_t *inode,
                   uint32_t offset, uint32_t size, uint8_t *buf);

/*
 * ext2_lookup_path -- resolve `path` to an inode number.
 *
 * Accepts absolute or relative paths ("/", "/bin/sh", "bin/sh"); "." and ".."
 * are resolved by the on-disk entries of the same name.  Writes the inode
 * number to `*out_ino` and, if `out_inode` is non-NULL, the inode itself.
 *
 * Matching is case-sensitive, with a case-insensitive fallback used only when
 * a directory contains no exact match -- Quilon's keyboard cannot type
 * uppercase, so "shell.elf" has to find SHELL.ELF.  See ci_name_matches in
 * ext2.c for the reasoning.
 *
 * Returns 0 on success, -1 if any path component does not exist or a
 * non-final component is not a directory.
 */
int ext2_lookup_path(ext2_ctx_t *fs, const char *path,
                     uint32_t *out_ino, ext2_inode_t *out_inode);

/*
 * ext2_vfs_ops -- VFS driver vtable for ext2 (read-only).
 *
 * Usage:
 *   ext2_ctx_t fs = { .sector_read = my_read, .ctx = my_ctx };
 *   if (ext2_mount(&fs) == 0)
 *       vfs_mount(&ext2_vfs_ops, &fs);
 */
extern const vfs_ops_t ext2_vfs_ops;

#endif /* _KERNEL_EXT2_H */
