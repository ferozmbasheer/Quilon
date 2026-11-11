/*
 * Quilon OS -- ext2 Driver Unit Tests (section 16.2)
 *
 * Tests kernel/kernel/ext2.c against a hand-crafted 32-block ext2 image held
 * in a static byte array.  No hardware, no ATA, no mke2fs.
 *
 * Test image layout (32 blocks x 1024 bytes = 32 KiB, one block group)
 * --------------------------------------------------------------------
 *   Block  0      Boot block (unused)
 *   Block  1      Superblock
 *   Block  2      Block group descriptor table (1 descriptor)
 *   Block  3      Block bitmap   (contents irrelevant -- read-only driver)
 *   Block  4      Inode bitmap   (ditto)
 *   Blocks 5-6    Inode table    (16 inodes x 128 bytes = 2048 bytes)
 *   Block  7      Root directory data          (inode 2)
 *   Block  8      "hello.txt" data             (inode 11)
 *   Block  9      "sub" directory data         (inode 12)
 *   Block 10      "sub/deep.txt" data          (inode 13)
 *   Blocks 11-22  "big.bin" direct blocks 0-11 (inode 14)
 *   Block 23      "big.bin" singly indirect block
 *   Block 24      "big.bin" file block 12 (reached via the indirect block)
 *   Block 25      "longfilename_over_12.txt" data (inode 15)
 *
 * Superblock values:
 *   s_inodes_count     = 16      s_blocks_count     = 32
 *   s_first_data_block = 1       s_log_block_size   = 0  (1024-byte blocks)
 *   s_blocks_per_group = 8192    s_inodes_per_group = 16
 *   s_rev_level        = 1       s_inode_size       = 128
 *
 * big.bin is 13 blocks long, which forces the singly indirect path for its
 * last block, and its direct block 5 is deliberately a hole (pointer 0) so
 * the sparse-read path is covered too.
 *
 * Build & run:  cd tests && make
 */

#include "framework.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/ext2.h>
#include <kernel/vfs.h>

/* -- In-memory disk image -------------------------------------------------- */

#define BLK          1024u
#define DISK_BLOCKS  32u
#define DISK_BYTES   (DISK_BLOCKS * BLK)

/* Block numbers used by the image (see the header comment). */
#define B_SUPER       1u
#define B_BGD         2u
#define B_INODE_TAB   5u
#define B_ROOT_DIR    7u
#define B_HELLO       8u
#define B_SUB_DIR     9u
#define B_DEEP       10u
#define B_BIG_FIRST  11u   /* 11..22 = big.bin direct blocks 0..11 */
#define B_BIG_IND    23u
#define B_BIG_12     24u
#define B_LONGNAME   25u

/* Inode numbers used by the image. */
#define I_ROOT        2u
#define I_HELLO      11u
#define I_SUB        12u
#define I_DEEP       13u
#define I_BIG        14u
#define I_LONG       15u

#define HELLO_TEXT  "Hello from ext2!"
#define DEEP_TEXT   "deep file contents"
#define LONG_TEXT   "long!"
#define LONG_NAME   "longfilename_over_12.txt"

/* big.bin: 13 blocks, block i filled with byte 0x40+i (block 5 is a hole). */
#define BIG_BLOCKS   13u
#define BIG_SIZE     (BIG_BLOCKS * BLK)
#define BIG_HOLE_IDX 5u

static uint8_t disk[DISK_BYTES];

/* -- Little-endian writers ------------------------------------------------- */

static void put16(uint32_t off, uint16_t v)
{
    disk[off]     = (uint8_t)(v & 0xFF);
    disk[off + 1] = (uint8_t)(v >> 8);
}

static void put32(uint32_t off, uint32_t v)
{
    disk[off]     = (uint8_t)(v & 0xFF);
    disk[off + 1] = (uint8_t)((v >> 8)  & 0xFF);
    disk[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    disk[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}

/* Byte offset of inode `ino` in the image (inode numbers start at 1). */
static uint32_t inode_off(uint32_t ino)
{
    return B_INODE_TAB * BLK + (ino - 1) * 128u;
}

/*
 * write_inode -- fill in the fields of inode `ino` the driver reads.
 *
 * `blocks` holds up to 15 entries copied straight into i_block[]: indices
 * 0-11 are direct, 12 is the singly indirect pointer.
 */
static void write_inode(uint32_t ino, uint16_t mode, uint32_t size,
                        const uint32_t *blocks, uint32_t nblocks)
{
    uint32_t o = inode_off(ino);
    put16(o + 0,  mode);                       /* i_mode        */
    put32(o + 4,  size);                       /* i_size        */
    put16(o + 26, 1);                          /* i_links_count */
    put32(o + 28, (size + 511u) / 512u);       /* i_blocks      */
    for (uint32_t i = 0; i < nblocks && i < 15; i++)
        put32(o + 40 + i * 4u, blocks[i]);     /* i_block[i]    */
}

/*
 * add_dirent -- append a directory entry at `*off` inside directory block
 * `block`.  `rec_len` is the caller's choice so the last entry in a block
 * can be padded out to the block boundary, as ext2 requires.
 */
static void add_dirent(uint32_t block, uint32_t *off, uint32_t ino,
                       const char *name, uint16_t rec_len)
{
    uint32_t base   = block * BLK + *off;
    uint32_t nlen   = (uint32_t)strlen(name);
    put32(base + 0, ino);
    put16(base + 4, rec_len);
    disk[base + 6]  = (uint8_t)nlen;
    disk[base + 7]  = 0;             /* file_type: driver reads the inode */
    memcpy(&disk[base + 8], name, nlen);
    *off += rec_len;
}

static void build_image(void)
{
    memset(disk, 0, sizeof(disk));

    /* -- Block 1: superblock (at byte offset 1024) -- */
    uint32_t sb = B_SUPER * BLK;
    put32(sb + 0,  16);          /* s_inodes_count     */
    put32(sb + 4,  DISK_BLOCKS); /* s_blocks_count     */
    put32(sb + 20, 1);           /* s_first_data_block */
    put32(sb + 24, 0);           /* s_log_block_size -> 1024 */
    put32(sb + 32, 8192);        /* s_blocks_per_group */
    put32(sb + 40, 16);          /* s_inodes_per_group */
    put16(sb + 56, 0xEF53);      /* s_magic            */
    put32(sb + 76, 1);           /* s_rev_level        */
    put16(sb + 88, 128);         /* s_inode_size       */
    put32(sb + 96, 0x0002);      /* s_feature_incompat: FILETYPE */

    /* -- Block 2: the one block group descriptor -- */
    uint32_t bgd = B_BGD * BLK;
    put32(bgd + 0, 3);           /* bg_block_bitmap */
    put32(bgd + 4, 4);           /* bg_inode_bitmap */
    put32(bgd + 8, B_INODE_TAB); /* bg_inode_table  */

    /* -- Inodes -- */
    uint32_t b;

    b = B_ROOT_DIR;
    write_inode(I_ROOT, EXT2_S_IFDIR | 0755, BLK, &b, 1);

    b = B_HELLO;
    write_inode(I_HELLO, EXT2_S_IFREG | 0644,
                (uint32_t)strlen(HELLO_TEXT), &b, 1);

    b = B_SUB_DIR;
    write_inode(I_SUB, EXT2_S_IFDIR | 0755, BLK, &b, 1);

    b = B_DEEP;
    write_inode(I_DEEP, EXT2_S_IFREG | 0644,
                (uint32_t)strlen(DEEP_TEXT), &b, 1);

    b = B_LONGNAME;
    write_inode(I_LONG, EXT2_S_IFREG | 0644,
                (uint32_t)strlen(LONG_TEXT), &b, 1);

    /* big.bin: 12 direct pointers (one of them a hole) + the indirect one. */
    uint32_t big_blocks[13];
    for (uint32_t i = 0; i < 12; i++)
        big_blocks[i] = (i == BIG_HOLE_IDX) ? 0 : (B_BIG_FIRST + i);
    big_blocks[EXT2_IND_BLOCK] = B_BIG_IND;
    write_inode(I_BIG, EXT2_S_IFREG | 0644, BIG_SIZE, big_blocks, 13);

    /* -- Block 7: root directory data --
     * rec_len values must tile the block exactly; the last entry absorbs
     * the remaining bytes.  Running total: 12+12+20+12+32 = 88. */
    uint32_t off = 0;
    add_dirent(B_ROOT_DIR, &off, I_ROOT,  ".",       12);
    add_dirent(B_ROOT_DIR, &off, I_ROOT,  "..",      12);
    add_dirent(B_ROOT_DIR, &off, I_HELLO, "hello.txt", 20);
    add_dirent(B_ROOT_DIR, &off, I_SUB,   "sub",     12);
    add_dirent(B_ROOT_DIR, &off, I_LONG,  LONG_NAME, 32);
    /* A pair differing only by case, to prove an exact match beats the
     * case-insensitive fallback.  They reuse existing inodes -- the tests
     * only care which inode number comes back. */
    add_dirent(B_ROOT_DIR, &off, I_DEEP,  "case.txt", 16);
    add_dirent(B_ROOT_DIR, &off, I_HELLO, "CASE.TXT", 16);
    add_dirent(B_ROOT_DIR, &off, I_BIG,   "big.bin", (uint16_t)(BLK - off));

    /* -- Block 9: "sub" directory data -- */
    off = 0;
    add_dirent(B_SUB_DIR, &off, I_SUB,  ".",  12);
    add_dirent(B_SUB_DIR, &off, I_ROOT, "..", 12);
    add_dirent(B_SUB_DIR, &off, I_DEEP, "deep.txt", (uint16_t)(BLK - off));

    /* -- File data -- */
    memcpy(&disk[B_HELLO * BLK], HELLO_TEXT, strlen(HELLO_TEXT));
    memcpy(&disk[B_DEEP  * BLK], DEEP_TEXT,  strlen(DEEP_TEXT));
    memcpy(&disk[B_LONGNAME * BLK], LONG_TEXT, strlen(LONG_TEXT));

    /* big.bin: file block i is filled with the byte 0x40 + i.  Block
     * BIG_HOLE_IDX has no disk block at all and must read back as zeroes. */
    for (uint32_t i = 0; i < 12; i++) {
        if (i == BIG_HOLE_IDX) continue;
        memset(&disk[(B_BIG_FIRST + i) * BLK], (int)(0x40 + i), BLK);
    }
    put32(B_BIG_IND * BLK, B_BIG_12);          /* indirect[0] -> block 24 */
    memset(&disk[B_BIG_12 * BLK], 0x40 + 12, BLK);
}

/* -- RAM-backed sector back-end -------------------------------------------- */

static int ram_read(void *ctx, uint32_t lba, void *buf)
{
    (void)ctx;
    if ((lba + 1) * 512u > DISK_BYTES) return -1;
    memcpy(buf, &disk[lba * 512u], 512);
    return 0;
}

/* Mount a fresh context over the image.  Returns the ext2_mount() result. */
static int mount_fs(ext2_ctx_t *fs)
{
    memset(fs, 0, sizeof(*fs));
    fs->sector_read = ram_read;
    fs->ctx         = (void *)0;
    return ext2_mount(fs);
}

/* ==========================================================================
 * 1. Pure helpers (no I/O)
 * ========================================================================== */

static void test_pure_helpers(void)
{
    ASSERT_EQ(ext2_block_size_from_log(0), 1024u, "log 0 -> 1024-byte blocks");
    ASSERT_EQ(ext2_block_size_from_log(1), 2048u, "log 1 -> 2048-byte blocks");
    ASSERT_EQ(ext2_block_size_from_log(2), 4096u, "log 2 -> 4096-byte blocks");
    ASSERT_EQ(ext2_block_size_from_log(3), 0u,
              "log 3 (8 KiB) rejected as unsupported");

    /* Inode 1 is the first inode of group 0; inode ipg+1 starts group 1. */
    ASSERT_EQ(ext2_inode_group(1, 16),  0u, "inode 1 is in group 0");
    ASSERT_EQ(ext2_inode_group(16, 16), 0u, "inode 16 is the last of group 0");
    ASSERT_EQ(ext2_inode_group(17, 16), 1u, "inode 17 starts group 1");
    ASSERT_EQ(ext2_inode_group(33, 16), 2u, "inode 33 is in group 2");
    ASSERT_EQ(ext2_inode_group(0, 16),  0u, "inode 0 is invalid -> group 0");

    ASSERT_EQ(ext2_inode_index(1, 16),  0u,  "inode 1 is index 0");
    ASSERT_EQ(ext2_inode_index(16, 16), 15u, "inode 16 is index 15");
    ASSERT_EQ(ext2_inode_index(17, 16), 0u,  "inode 17 is index 0 of group 1");

    /* first_data_block blocks sit outside any group. */
    ASSERT_EQ(ext2_group_count(32, 1, 8192), 1u, "32-block fs has 1 group");
    ASSERT_EQ(ext2_group_count(8193, 1, 8192), 1u,
              "8193 blocks with first_data_block 1 is exactly 1 group");
    ASSERT_EQ(ext2_group_count(8194, 1, 8192), 2u,
              "one block past a full group makes 2 groups");
    ASSERT_EQ(ext2_group_count(32, 1, 0), 0u, "zero blocks_per_group -> 0");

    /* rec_len = (8 + name_len) rounded up to a multiple of 4. */
    ASSERT_EQ(ext2_dirent_min_len(1),  12, "name_len 1 needs rec_len 12");
    ASSERT_EQ(ext2_dirent_min_len(2),  12, "name_len 2 needs rec_len 12");
    ASSERT_EQ(ext2_dirent_min_len(4),  12, "name_len 4 needs rec_len 12");
    ASSERT_EQ(ext2_dirent_min_len(5),  16, "name_len 5 needs rec_len 16");
    ASSERT_EQ(ext2_dirent_min_len(9),  20, "name_len 9 needs rec_len 20");
    ASSERT_EQ(ext2_dirent_min_len(24), 32, "name_len 24 needs rec_len 32");
}

/* ==========================================================================
 * 2. Mount / probe
 * ========================================================================== */

static void test_mount_parses_superblock(void)
{
    build_image();
    ext2_ctx_t fs;
    ASSERT_EQ(mount_fs(&fs), 0, "ext2_mount succeeds on a valid image");

    ASSERT_EQ(fs.block_size,       1024u, "block_size is 1024");
    ASSERT_EQ(fs.sectors_per_block,   2u, "1024-byte block = 2 sectors");
    ASSERT_EQ(fs.inodes_count,       16u, "inodes_count from superblock");
    ASSERT_EQ(fs.blocks_count,       32u, "blocks_count from superblock");
    ASSERT_EQ(fs.first_data_block,    1u, "first_data_block is 1 at 1 KiB");
    ASSERT_EQ(fs.blocks_per_group, 8192u, "blocks_per_group from superblock");
    ASSERT_EQ(fs.inodes_per_group,   16u, "inodes_per_group from superblock");
    ASSERT_EQ(fs.inode_size,        128u, "inode_size from superblock");
    ASSERT_EQ(fs.rev_level,           1u, "rev_level is 1");
    ASSERT_EQ(fs.group_count,         1u, "image has one block group");
    ASSERT_EQ(fs.bgd_block,           2u, "BGD table follows the superblock");
}

static void test_probe(void)
{
    build_image();
    ext2_ctx_t fs;
    memset(&fs, 0, sizeof fs);
    fs.sector_read = ram_read;
    ASSERT_EQ(ext2_probe(&fs), 1, "ext2_probe detects the ext2 magic");

    put16(B_SUPER * BLK + 56, 0x1234);
    ASSERT_EQ(ext2_probe(&fs), 0, "ext2_probe rejects a non-ext2 volume");
}

static void test_mount_rejects_bad_images(void)
{
    /* Wrong magic. */
    build_image();
    put16(B_SUPER * BLK + 56, 0xBEEF);
    ext2_ctx_t fs;
    ASSERT_EQ(mount_fs(&fs), -1, "mount rejects a bad magic");

    /* Block size above the 4 KiB limit. */
    build_image();
    put32(B_SUPER * BLK + 24, 3);
    ASSERT_EQ(mount_fs(&fs), -1, "mount rejects an 8 KiB block size");

    /* An inode size that does not divide the block size. */
    build_image();
    put16(B_SUPER * BLK + 88, 100);
    ASSERT_EQ(mount_fs(&fs), -1, "mount rejects a non-dividing inode size");

    /* Degenerate geometry. */
    build_image();
    put32(B_SUPER * BLK + 40, 0);
    ASSERT_EQ(mount_fs(&fs), -1, "mount rejects inodes_per_group = 0");

    build_image();
    put32(B_SUPER * BLK + 32, 0);
    ASSERT_EQ(mount_fs(&fs), -1, "mount rejects blocks_per_group = 0");
}

static void test_mount_rev0_uses_128_byte_inodes(void)
{
    /* Revision 0 has no s_inode_size field: a stale value there must be
     * ignored in favour of the fixed 128-byte inode. */
    build_image();
    put32(B_SUPER * BLK + 76, 0);     /* s_rev_level = 0    */
    put16(B_SUPER * BLK + 88, 256);   /* bogus s_inode_size */

    ext2_ctx_t fs;
    ASSERT_EQ(mount_fs(&fs), 0, "rev 0 image mounts");
    ASSERT_EQ(fs.inode_size, 128u, "rev 0 forces a 128-byte inode size");
    ASSERT_EQ(fs.feature_incompat, 0u, "rev 0 reports no incompat features");
}

/* ==========================================================================
 * 3. Inode reading
 * ========================================================================== */

static void test_read_inode(void)
{
    build_image();
    ext2_ctx_t fs;
    mount_fs(&fs);

    ext2_inode_t in;
    ASSERT_EQ(ext2_read_inode(&fs, I_ROOT, &in), 0, "root inode reads back");
    ASSERT_EQ(in.i_mode & EXT2_S_IFMT, EXT2_S_IFDIR, "root inode is a dir");
    ASSERT_EQ(in.i_size, BLK, "root directory is one block long");
    ASSERT_EQ(in.i_block[0], B_ROOT_DIR, "root data block pointer");

    ASSERT_EQ(ext2_read_inode(&fs, I_HELLO, &in), 0, "hello inode reads back");
    ASSERT_EQ(in.i_mode & EXT2_S_IFMT, EXT2_S_IFREG, "hello is a regular file");
    ASSERT_EQ(in.i_size, (uint32_t)strlen(HELLO_TEXT), "hello size matches");

    ASSERT_EQ(ext2_read_inode(&fs, 0, &in), -1, "inode 0 is rejected");
    ASSERT_EQ(ext2_read_inode(&fs, 999, &in), -1,
              "an inode past inodes_count is rejected");
}

/* ==========================================================================
 * 4. Block mapping (direct / indirect / holes)
 * ========================================================================== */

static void test_bmap(void)
{
    build_image();
    ext2_ctx_t fs;
    mount_fs(&fs);

    ext2_inode_t big;
    ext2_read_inode(&fs, I_BIG, &big);

    uint32_t blk = 0xFFFFFFFFu;
    ASSERT_EQ(ext2_bmap(&fs, &big, 0, &blk), 0, "bmap block 0 succeeds");
    ASSERT_EQ(blk, B_BIG_FIRST, "block 0 is the first direct pointer");

    ASSERT_EQ(ext2_bmap(&fs, &big, 11, &blk), 0, "bmap block 11 succeeds");
    ASSERT_EQ(blk, B_BIG_FIRST + 11u, "block 11 is the last direct pointer");

    ASSERT_EQ(ext2_bmap(&fs, &big, BIG_HOLE_IDX, &blk), 0,
              "bmap of a hole succeeds");
    ASSERT_EQ(blk, 0u, "a hole maps to block 0");

    ASSERT_EQ(ext2_bmap(&fs, &big, 12, &blk), 0,
              "bmap block 12 goes through the indirect block");
    ASSERT_EQ(blk, B_BIG_12, "block 12 comes from indirect[0]");

    /* Past the end of the indirect block's populated entries: still a
     * successful mapping, just to a zero (unallocated) block. */
    ASSERT_EQ(ext2_bmap(&fs, &big, 13, &blk), 0, "bmap block 13 succeeds");
    ASSERT_EQ(blk, 0u, "unallocated indirect entry maps to 0");

    /* A file with no indirect pointer at all. */
    ext2_inode_t hello;
    ext2_read_inode(&fs, I_HELLO, &hello);
    ASSERT_EQ(ext2_bmap(&fs, &hello, 12, &blk), 0,
              "bmap past a missing indirect pointer succeeds");
    ASSERT_EQ(blk, 0u, "missing indirect pointer maps to 0");

    /* Triply indirect is out of scope: 12 + 256 + 256*256 = 65804. */
    ASSERT_EQ(ext2_bmap(&fs, &big, 65804u, &blk), -1,
              "triply indirect block index is rejected");
}

/* ==========================================================================
 * 5. File data
 * ========================================================================== */

static void test_read_data(void)
{
    build_image();
    ext2_ctx_t fs;
    mount_fs(&fs);

    ext2_inode_t in;
    ext2_read_inode(&fs, I_HELLO, &in);

    uint8_t buf[64];
    memset(buf, 0, sizeof buf);
    int n = ext2_read_data(&fs, &in, 0, sizeof buf, buf);
    ASSERT_EQ(n, (int)strlen(HELLO_TEXT), "read clamps to the file size");
    ASSERT_MEM_EQ(buf, HELLO_TEXT, strlen(HELLO_TEXT), "file contents match");

    /* Partial read from an offset. */
    memset(buf, 0, sizeof buf);
    n = ext2_read_data(&fs, &in, 6, 4, buf);
    ASSERT_EQ(n, 4, "offset read returns the requested length");
    ASSERT_MEM_EQ(buf, HELLO_TEXT + 6, 4, "offset read returns the right bytes");

    /* At and past EOF. */
    ASSERT_EQ(ext2_read_data(&fs, &in, (uint32_t)strlen(HELLO_TEXT), 8, buf), 0,
              "read at EOF returns 0");
    ASSERT_EQ(ext2_read_data(&fs, &in, 9999, 8, buf), 0,
              "read past EOF returns 0");
    ASSERT_EQ(ext2_read_data(&fs, &in, 0, 0, buf), 0, "zero-length read is 0");
}

static void test_read_data_crosses_blocks(void)
{
    build_image();
    ext2_ctx_t fs;
    mount_fs(&fs);

    ext2_inode_t big;
    ext2_read_inode(&fs, I_BIG, &big);

    /* Straddle the boundary between file blocks 0 and 1. */
    uint8_t buf[8];
    int n = ext2_read_data(&fs, &big, BLK - 4, sizeof buf, buf);
    ASSERT_EQ(n, 8, "read across a block boundary returns the full length");
    ASSERT_EQ(buf[0], 0x40, "bytes before the boundary come from block 0");
    ASSERT_EQ(buf[3], 0x40, "last byte of block 0");
    ASSERT_EQ(buf[4], 0x41, "first byte of block 1");
    ASSERT_EQ(buf[7], 0x41, "bytes after the boundary come from block 1");

    /* A hole reads as zeroes even though the file size covers it. */
    memset(buf, 0xAA, sizeof buf);
    n = ext2_read_data(&fs, &big, BIG_HOLE_IDX * BLK, sizeof buf, buf);
    ASSERT_EQ(n, 8, "reading a hole returns the requested length");
    ASSERT_EQ(buf[0], 0x00, "a hole reads as zeroes");
    ASSERT_EQ(buf[7], 0x00, "a hole reads as zeroes throughout");

    /* The last block is only reachable via the indirect block. */
    n = ext2_read_data(&fs, &big, 12u * BLK, sizeof buf, buf);
    ASSERT_EQ(n, 8, "read from the indirect-mapped block succeeds");
    ASSERT_EQ(buf[0], 0x4C, "indirect-mapped block holds 0x40 + 12");

    /* Reading the whole file ends exactly at i_size. */
    static uint8_t big_buf[BIG_SIZE + 16];
    n = ext2_read_data(&fs, &big, 0, sizeof big_buf, big_buf);
    ASSERT_EQ(n, (int)BIG_SIZE, "whole-file read is clamped to i_size");
    ASSERT_EQ(big_buf[BIG_SIZE - 1], 0x4C, "final byte of the last block");
}

/* ==========================================================================
 * 6. Path resolution
 * ========================================================================== */

static void test_lookup_path(void)
{
    build_image();
    ext2_ctx_t fs;
    mount_fs(&fs);

    uint32_t     ino = 0;
    ext2_inode_t in;

    ASSERT_EQ(ext2_lookup_path(&fs, "/", &ino, &in), 0, "\"/\" resolves");
    ASSERT_EQ(ino, I_ROOT, "\"/\" is the root inode");
    ASSERT_EQ(in.i_mode & EXT2_S_IFMT, EXT2_S_IFDIR, "root is a directory");

    ASSERT_EQ(ext2_lookup_path(&fs, "/hello.txt", &ino, &in), 0,
              "/hello.txt resolves");
    ASSERT_EQ(ino, I_HELLO, "/hello.txt is inode 11");
    ASSERT_EQ(in.i_size, (uint32_t)strlen(HELLO_TEXT), "size comes back too");

    /* Real subdirectory traversal -- the thing FAT16 could not do. */
    ASSERT_EQ(ext2_lookup_path(&fs, "/sub/deep.txt", &ino, &in), 0,
              "/sub/deep.txt resolves through a subdirectory");
    ASSERT_EQ(ino, I_DEEP, "/sub/deep.txt is inode 13");

    /* Path spelling variations. */
    ASSERT_EQ(ext2_lookup_path(&fs, "sub/deep.txt", &ino, &in), 0,
              "a relative path resolves from the root");
    ASSERT_EQ(ino, I_DEEP, "relative path finds the same inode");
    ASSERT_EQ(ext2_lookup_path(&fs, "//sub//deep.txt", &ino, &in), 0,
              "repeated slashes are tolerated");
    ASSERT_EQ(ino, I_DEEP, "repeated slashes find the same inode");
    ASSERT_EQ(ext2_lookup_path(&fs, "/sub/", &ino, &in), 0,
              "a trailing slash is tolerated");
    ASSERT_EQ(ino, I_SUB, "/sub/ is the subdirectory inode");

    /* "." and ".." come from the on-disk entries. */
    ASSERT_EQ(ext2_lookup_path(&fs, "/sub/..", &ino, &in), 0, "/sub/.. resolves");
    ASSERT_EQ(ino, I_ROOT, "/sub/.. is the root inode");
    ASSERT_EQ(ext2_lookup_path(&fs, "/sub/.", &ino, &in), 0, "/sub/. resolves");
    ASSERT_EQ(ino, I_SUB, "/sub/. is the subdirectory inode");

    /* Long names are matched in full, not truncated. */
    ASSERT_EQ(ext2_lookup_path(&fs, "/" LONG_NAME, &ino, &in), 0,
              "a 24-character name resolves");
    ASSERT_EQ(ino, I_LONG, "long name finds inode 15");

    /* Failures. */
    ASSERT_EQ(ext2_lookup_path(&fs, "/nosuch", &ino, &in), -1,
              "a missing name fails");
    ASSERT_EQ(ext2_lookup_path(&fs, "/sub/nosuch", &ino, &in), -1,
              "a missing name in a subdirectory fails");
    ASSERT_EQ(ext2_lookup_path(&fs, "/hello.txt/x", &ino, &in), -1,
              "descending into a regular file fails");
    ASSERT_EQ(ext2_lookup_path(&fs, "/hello", &ino, &in), -1,
              "a name prefix does not match (exact length required)");
}

/*
 * Case handling: ext2 is case-sensitive, but Quilon's keyboard cannot type
 * uppercase, so a case-insensitive fallback applies when -- and only when --
 * a directory holds no exact match.
 */
static void test_lookup_case_fallback(void)
{
    build_image();
    ext2_ctx_t fs;
    mount_fs(&fs);

    uint32_t ino = 0;

    /* No entry is spelled "HELLO.TXT", so the fallback finds "hello.txt". */
    ASSERT_EQ(ext2_lookup_path(&fs, "/HELLO.TXT", &ino, (ext2_inode_t *)0), 0,
              "an uppercase spelling falls back to the lowercase entry");
    ASSERT_EQ(ino, I_HELLO, "the fallback resolves to the right inode");

    ASSERT_EQ(ext2_lookup_path(&fs, "/SUB/DEEP.TXT", &ino, (ext2_inode_t *)0), 0,
              "the fallback applies to every path component");
    ASSERT_EQ(ino, I_DEEP, "nested fallback resolves to the right inode");

    /* When both spellings exist, each must resolve to its own inode -- the
     * exact match has to beat the fallback even though "CASE.TXT" appears
     * later in the directory than "case.txt". */
    ASSERT_EQ(ext2_lookup_path(&fs, "/case.txt", &ino, (ext2_inode_t *)0), 0,
              "lowercase case.txt resolves");
    ASSERT_EQ(ino, I_DEEP, "case.txt matches exactly, not the uppercase twin");

    ASSERT_EQ(ext2_lookup_path(&fs, "/CASE.TXT", &ino, (ext2_inode_t *)0), 0,
              "uppercase CASE.TXT resolves");
    ASSERT_EQ(ino, I_HELLO, "CASE.TXT matches exactly, not the lowercase twin");

    /* The fallback must not turn a genuinely missing name into a match. */
    ASSERT_EQ(ext2_lookup_path(&fs, "/NOSUCH.TXT", &ino, (ext2_inode_t *)0), -1,
              "the fallback does not invent matches");
}

/* ==========================================================================
 * 7. VFS integration
 * ========================================================================== */

static void test_vfs_open_read_close(void)
{
    build_image();
    ext2_ctx_t fs;
    mount_fs(&fs);
    vfs_mount(&ext2_vfs_ops, &fs);
    ASSERT_EQ(vfs_mounted(), 1, "ext2 mounts into the VFS");

    int fd = vfs_open("/hello.txt");
    ASSERT(fd >= VFS_FD_BASE, "vfs_open returns a valid fd");

    char buf[64];
    memset(buf, 0, sizeof buf);
    int n = vfs_read(fd, buf, sizeof buf);
    ASSERT_EQ(n, (int)strlen(HELLO_TEXT), "vfs_read returns the file length");
    ASSERT_STR_EQ(buf, HELLO_TEXT, "vfs_read returns the file contents");
    ASSERT_EQ(vfs_read(fd, buf, sizeof buf), 0, "a second read hits EOF");
    ASSERT_EQ(vfs_close(fd), 0, "vfs_close succeeds");

    /* Through a subdirectory. */
    fd = vfs_open("/sub/deep.txt");
    ASSERT(fd >= VFS_FD_BASE, "vfs_open resolves a nested path");
    memset(buf, 0, sizeof buf);
    n = vfs_read(fd, buf, sizeof buf);
    ASSERT_EQ(n, (int)strlen(DEEP_TEXT), "nested file length");
    ASSERT_STR_EQ(buf, DEEP_TEXT, "nested file contents");
    vfs_close(fd);

    ASSERT_EQ(vfs_open("/nosuch.txt"), -1, "opening a missing file fails");
}

static void test_vfs_lseek_and_stat(void)
{
    build_image();
    ext2_ctx_t fs;
    mount_fs(&fs);
    vfs_mount(&ext2_vfs_ops, &fs);

    int fd = vfs_open("/hello.txt");
    ASSERT_EQ(vfs_lseek(fd, 6, VFS_SEEK_SET), 6, "lseek to offset 6");
    char buf[8];
    memset(buf, 0, sizeof buf);
    ASSERT_EQ(vfs_read(fd, buf, 4), 4, "read after lseek returns 4 bytes");
    ASSERT_MEM_EQ(buf, HELLO_TEXT + 6, 4, "read after lseek is correctly placed");
    vfs_close(fd);

    vfs_stat_t st;
    ASSERT_EQ(vfs_stat("/hello.txt", &st), 0, "stat on a file succeeds");
    ASSERT_EQ(st.type, VFS_TYPE_FILE, "stat reports a regular file");
    ASSERT_EQ(st.size, (uint32_t)strlen(HELLO_TEXT), "stat reports the size");

    ASSERT_EQ(vfs_stat("/sub", &st), 0, "stat on a directory succeeds");
    ASSERT_EQ(st.type, VFS_TYPE_DIR, "stat reports a directory");
    ASSERT_EQ(vfs_stat("/nosuch", &st), -1, "stat on a missing path fails");
}

static void test_vfs_readdir(void)
{
    build_image();
    ext2_ctx_t fs;
    mount_fs(&fs);
    vfs_mount(&ext2_vfs_ops, &fs);
    vfs_chdir("/");

    /* On-disk order: . .. hello.txt sub <long> case.txt CASE.TXT big.bin */
    vfs_dirent_t e;
    ASSERT_EQ(vfs_readdir(0, &e), 0, "readdir entry 0 succeeds");
    ASSERT_STR_EQ(e.name, ".", "entry 0 is \".\"");
    ASSERT_EQ(e.type, VFS_TYPE_DIR, "\".\" is a directory");

    ASSERT_EQ(vfs_readdir(1, &e), 0, "readdir entry 1 succeeds");
    ASSERT_STR_EQ(e.name, "..", "entry 1 is \"..\"");

    ASSERT_EQ(vfs_readdir(2, &e), 0, "readdir entry 2 succeeds");
    ASSERT_STR_EQ(e.name, "hello.txt", "entry 2 is hello.txt");
    ASSERT_EQ(e.type, VFS_TYPE_FILE, "hello.txt is a regular file");
    ASSERT_EQ(e.size, (uint32_t)strlen(HELLO_TEXT), "hello.txt size is right");

    ASSERT_EQ(vfs_readdir(3, &e), 0, "readdir entry 3 succeeds");
    ASSERT_STR_EQ(e.name, "sub", "entry 3 is sub");
    ASSERT_EQ(e.type, VFS_TYPE_DIR, "sub is a directory");

    /* Names longer than the VFS limit are truncated, not dropped. */
    ASSERT_EQ(vfs_readdir(4, &e), 0, "readdir entry 4 succeeds");
    char expect[VFS_NAME_MAX + 1];
    uint32_t keep = (uint32_t)strlen(LONG_NAME);
    if (keep > VFS_NAME_MAX) keep = VFS_NAME_MAX;
    memcpy(expect, LONG_NAME, keep);
    expect[keep] = '\0';
    ASSERT_STR_EQ(e.name, expect, "a long name is truncated to VFS_NAME_MAX");

    ASSERT_EQ(vfs_readdir(5, &e), 0, "readdir entry 5 succeeds");
    ASSERT_STR_EQ(e.name, "case.txt", "entry 5 is case.txt");
    ASSERT_EQ(vfs_readdir(6, &e), 0, "readdir entry 6 succeeds");
    ASSERT_STR_EQ(e.name, "CASE.TXT",
                  "entry 6 keeps the on-disk case of the name");

    ASSERT_EQ(vfs_readdir(7, &e), 0, "readdir entry 7 succeeds");
    ASSERT_STR_EQ(e.name, "big.bin", "entry 7 is big.bin");
    ASSERT_EQ(e.size, BIG_SIZE, "big.bin size is right");

    ASSERT_EQ(vfs_readdir(8, &e), -1, "readdir past the last entry fails");

    /* Listing a subdirectory follows the VFS working directory. */
    ASSERT_EQ(vfs_chdir("/sub"), 0, "chdir into /sub succeeds");
    ASSERT_EQ(vfs_readdir(2, &e), 0, "readdir in /sub succeeds");
    ASSERT_STR_EQ(e.name, "deep.txt", "third entry of /sub is deep.txt");
    ASSERT_EQ(vfs_readdir(3, &e), -1, "/sub has exactly three entries");
    vfs_chdir("/");

    ASSERT_EQ(vfs_chdir("/hello.txt"), -1, "chdir into a file fails");
}

static void test_vfs_is_read_only(void)
{
    build_image();
    ext2_ctx_t fs;
    mount_fs(&fs);
    vfs_mount(&ext2_vfs_ops, &fs);

    int fd = vfs_open("/hello.txt");
    ASSERT_EQ(vfs_write(fd, "x", 1), -1, "vfs_write is unsupported on ext2");
    vfs_close(fd);

    ASSERT_EQ(vfs_create("/new.txt"), -1, "vfs_create is unsupported");
    ASSERT_EQ(vfs_remove("/hello.txt"), -1, "vfs_remove is unsupported");
    ASSERT_EQ(vfs_mkdir("/newdir"), -1, "vfs_mkdir is unsupported");
    ASSERT_EQ(vfs_rename("/hello.txt", "/other.txt"), -1,
              "vfs_rename is unsupported");
}

/*
 * fsinfo (section 16.2) -- backs the `fs` shell command and SYS_FSINFO.
 */
static void test_vfs_fsinfo(void)
{
    build_image();
    ext2_ctx_t fs;
    mount_fs(&fs);
    vfs_mount(&ext2_vfs_ops, &fs);

    vfs_fsinfo_t fi;
    ASSERT_EQ(vfs_fsinfo(&fi), 0, "vfs_fsinfo succeeds on a mounted ext2");
    ASSERT_STR_EQ(fi.type, "ext2", "fsinfo reports the ext2 type");
    ASSERT_EQ(fi.block_size, 1024u, "fsinfo reports the block size");
    ASSERT_EQ(fi.total_blocks, DISK_BLOCKS, "fsinfo reports the volume size");
    ASSERT_EQ(fi.name_max, (uint32_t)VFS_NAME_MAX,
              "fsinfo reports the readdir name limit");

    ASSERT_EQ(vfs_fsinfo((vfs_fsinfo_t *)0), -1, "vfs_fsinfo rejects NULL");
}

/* ==========================================================================
 * 8. I/O error propagation
 * ========================================================================== */

/* A back-end that fails every read, to check errors are not swallowed. */
static int failing_read(void *ctx, uint32_t lba, void *buf)
{
    (void)ctx; (void)lba; (void)buf;
    return -1;
}

static void test_io_errors(void)
{
    build_image();
    ext2_ctx_t fs;
    memset(&fs, 0, sizeof fs);
    fs.sector_read = failing_read;
    ASSERT_EQ(ext2_mount(&fs), -1, "mount fails when the disk read fails");
    ASSERT_EQ(ext2_probe(&fs), 0,  "probe reports no ext2 when reads fail");

    /* Mount for real, then break the back-end underneath the driver. */
    mount_fs(&fs);
    ext2_inode_t in;
    ASSERT_EQ(ext2_read_inode(&fs, I_HELLO, &in), 0, "inode read works first");
    fs.sector_read = failing_read;
    ASSERT_EQ(ext2_read_inode(&fs, I_HELLO, &in), -1,
              "inode read fails once the disk fails");

    uint32_t ino;
    ASSERT_EQ(ext2_lookup_path(&fs, "/hello.txt", &ino, (ext2_inode_t *)0), -1,
              "lookup fails once the disk fails");

    ASSERT_EQ(ext2_mount((ext2_ctx_t *)0), -1, "mount rejects a NULL context");
}

/*
 * A directory block with a bogus rec_len must terminate the walk instead of
 * looping forever or reading past the end of the block.
 */
static void test_corrupt_dirent_terminates(void)
{
    build_image();
    /* Entry 0 ("."), rec_len = 0 -> below the 12-byte minimum. */
    put16(B_ROOT_DIR * BLK + 4, 0);

    ext2_ctx_t fs;
    mount_fs(&fs);
    uint32_t ino;
    ASSERT_EQ(ext2_lookup_path(&fs, "/hello.txt", &ino, (ext2_inode_t *)0), -1,
              "a zero rec_len stops the directory walk");

    build_image();
    /* rec_len that runs past the end of the block. */
    put16(B_ROOT_DIR * BLK + 4, (uint16_t)(BLK + 8));
    mount_fs(&fs);
    ASSERT_EQ(ext2_lookup_path(&fs, "/hello.txt", &ino, (ext2_inode_t *)0), -1,
              "an over-long rec_len stops the directory walk");
}

/* ========================================================================== */

int main(void)
{
    /* pure helpers */
    RUN_SUITE(test_pure_helpers);

    /* mount */
    RUN_SUITE(test_mount_parses_superblock);
    RUN_SUITE(test_probe);
    RUN_SUITE(test_mount_rejects_bad_images);
    RUN_SUITE(test_mount_rev0_uses_128_byte_inodes);

    /* inodes and blocks */
    RUN_SUITE(test_read_inode);
    RUN_SUITE(test_bmap);

    /* file data */
    RUN_SUITE(test_read_data);
    RUN_SUITE(test_read_data_crosses_blocks);

    /* paths */
    RUN_SUITE(test_lookup_path);
    RUN_SUITE(test_lookup_case_fallback);

    /* VFS integration */
    RUN_SUITE(test_vfs_open_read_close);
    RUN_SUITE(test_vfs_lseek_and_stat);
    RUN_SUITE(test_vfs_readdir);
    RUN_SUITE(test_vfs_is_read_only);
    RUN_SUITE(test_vfs_fsinfo);

    /* robustness */
    RUN_SUITE(test_io_errors);
    RUN_SUITE(test_corrupt_dirent_terminates);

    TEST_SUMMARY();
}
