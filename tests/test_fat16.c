/*
 * Quilon OS — FAT16 Driver Unit Tests
 *
 * Tests kernel/kernel/fat16.c using a hand-crafted 10-sector FAT16 image
 * stored in a static byte array.  No hardware, no ATA.
 *
 * Test image layout (10 × 512 = 5120 bytes)
 * ──────────────────────────────────────────
 *   Sector 0  Boot sector / BPB
 *   Sector 1  FAT (entries 0-3 used, 4-9 free)
 *   Sector 2  Root directory (entries: HELLO.TXT, WORLD.TXT, then 0x00 end)
 *   Sector 3  Cluster 2 data  →  "Hello, World!"  (13 bytes)
 *   Sector 4  Cluster 3 data  →  "Hello World!"   (12 bytes)
 *   Sectors 5-9  Clusters 4-8  (free — used by write tests)
 *
 * BPB values:
 *   bytes_per_sector    = 512
 *   sectors_per_cluster = 1
 *   reserved_sectors    = 1
 *   num_fats            = 1
 *   root_entry_count    = 16
 *   sectors_per_fat     = 1
 *
 * Derived:
 *   fat_lba      = 1
 *   root_dir_lba = 2   (1 + 1×1)
 *   data_lba     = 3   (2 + 16×32/512 = 2 + 1)
 *   cluster2_lba = 3   (data_lba + (2-2)×1)
 *   cluster3_lba = 4
 *   cluster4_lba = 5   (first free — allocated by create/write tests)
 *
 * Build & run:  cd tests && make
 */

#include "framework.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/fat16.h>
#include <kernel/vfs.h>

/* ── In-memory disk image ────────────────────────────────────────────────── */

#define DISK_SECTORS 10
static uint8_t disk[DISK_SECTORS * 512];

static void build_image(void)
{
    memset(disk, 0, sizeof(disk));

    /* ── Sector 0: Boot sector / BPB ── */
    uint8_t *b = disk;
    /* JMP short + NOP */
    b[0x00] = 0xEB; b[0x01] = 0x58; b[0x02] = 0x90;
    /* OEM name */
    b[0x03] = 'Q'; b[0x04] = 'U'; b[0x05] = 'I'; b[0x06] = 'L';
    b[0x07] = 'O'; b[0x08] = 'N'; b[0x09] = ' '; b[0x0A] = ' ';
    /* bytes_per_sector = 512 = 0x0200 (little-endian) */
    b[0x0B] = 0x00; b[0x0C] = 0x02;
    /* sectors_per_cluster = 1 */
    b[0x0D] = 0x01;
    /* reserved_sectors = 1 */
    b[0x0E] = 0x01; b[0x0F] = 0x00;
    /* num_fats = 1 */
    b[0x10] = 0x01;
    /* root_entry_count = 16 */
    b[0x11] = 0x10; b[0x12] = 0x00;
    /* total_sectors_16 = 10 */
    b[0x13] = 0x0A; b[0x14] = 0x00;
    /* media_type = 0xF8 (fixed disk) */
    b[0x15] = 0xF8;
    /* sectors_per_fat = 1 */
    b[0x16] = 0x01; b[0x17] = 0x00;
    /* sectors_per_track = 63 */
    b[0x18] = 0x3F; b[0x19] = 0x00;
    /* num_heads = 255 */
    b[0x1A] = 0xFF; b[0x1B] = 0x00;
    /* hidden_sectors = 0, total_sectors_32 = 0: already zeroed */
    /* Boot signature */
    b[0x1FE] = 0x55; b[0x1FF] = 0xAA;

    /* ── Sector 1: FAT ── */
    uint8_t *fat = disk + 512;
    /* entry[0]: 0xFFF8 media descriptor copy */
    fat[0] = 0xF8; fat[1] = 0xFF;
    /* entry[1]: 0xFFFF reserved */
    fat[2] = 0xFF; fat[3] = 0xFF;
    /* entry[2]: 0xFFFF end-of-chain (HELLO.TXT) */
    fat[4] = 0xFF; fat[5] = 0xFF;
    /* entry[3]: 0xFFFF end-of-chain (WORLD.TXT) */
    fat[6] = 0xFF; fat[7] = 0xFF;

    /* ── Sector 2: Root directory ── */
    uint8_t *root = disk + 2 * 512;

    /* Entry 0: HELLO.TXT, cluster=2, size=13
     * FAT dirent layout:
     *   [0..7]   name (space-padded)
     *   [8..10]  ext
     *   [11]     attr
     *   [12..25] reserved/time/date fields
     *   [26..27] first_cluster (little-endian)
     *   [28..31] file_size     (little-endian)
     */
    memcpy(root +  0, "HELLO   ", 8);
    memcpy(root +  8, "TXT",      3);
    root[11] = 0x20;                     /* archive attribute */
    root[26] = 0x02; root[27] = 0x00;   /* first_cluster = 2 */
    root[28] = 0x0D;                     /* file_size = 13    */

    /* Entry 1: WORLD.TXT, cluster=3, size=12 (at byte offset 32) */
    memcpy(root + 32, "WORLD   ", 8);
    memcpy(root + 40, "TXT",      3);
    root[43] = 0x20;
    root[58] = 0x03; root[59] = 0x00;   /* first_cluster = 3 */
    root[60] = 0x0C;                     /* file_size = 12    */

    /* Entry 2 (byte offset 64): 0x00 = end of directory */
    root[64] = 0x00;

    /* ── Sector 3: cluster 2 data (HELLO.TXT content) ── */
    memcpy(disk + 3 * 512, "Hello, World!", 13);

    /* ── Sector 4: cluster 3 data (WORLD.TXT content) ── */
    memcpy(disk + 4 * 512, "Hello World!", 12);
}

/* ── Mock sector reader ──────────────────────────────────────────────────── */

static int mock_sector_read(void *ctx, uint32_t lba, void *buf)
{
    (void)ctx;
    if (lba >= DISK_SECTORS) return -1;
    memcpy(buf, disk + lba * 512, 512);
    return 0;
}

static int mock_sector_write(void *ctx, uint32_t lba, const void *buf)
{
    (void)ctx;
    if (lba >= DISK_SECTORS) return -1;
    memcpy(disk + lba * 512, buf, 512);
    return 0;
}

/* ── Shared test context ──────────────────────────────────────────────────── */

static fat16_ctx_t g_fs;

static void setup_fs(void)
{
    g_fs.sector_read  = mock_sector_read;
    g_fs.sector_write = (void *)0;
    g_fs.ctx          = (void *)0;
}

/* Reset disk to a clean state and configure read+write callbacks. */
static void setup_write_fs(void)
{
    build_image();
    g_fs.sector_read  = mock_sector_read;
    g_fs.sector_write = mock_sector_write;
    g_fs.ctx          = (void *)0;
    fat16_mount(&g_fs);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 1. fat16_mount — BPB parsing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_mount_succeeds(void)
{
    setup_fs();
    int r = fat16_mount(&g_fs);
    ASSERT_EQ(r, 0, "fat16_mount succeeds on valid image");
}

static void test_mount_parses_bpb(void)
{
    setup_fs();
    fat16_mount(&g_fs);

    ASSERT_EQ(g_fs.bytes_per_sector,    (uint16_t)512, "bytes_per_sector = 512");
    ASSERT_EQ(g_fs.sectors_per_cluster, (uint8_t)1,    "sectors_per_cluster = 1");
    ASSERT_EQ(g_fs.reserved_sectors,    (uint16_t)1,   "reserved_sectors = 1");
    ASSERT_EQ(g_fs.num_fats,            (uint8_t)1,    "num_fats = 1");
    ASSERT_EQ(g_fs.root_entry_count,    (uint16_t)16,  "root_entry_count = 16");
    ASSERT_EQ(g_fs.sectors_per_fat,     (uint16_t)1,   "sectors_per_fat = 1");
}

static void test_mount_derives_lba_addresses(void)
{
    setup_fs();
    fat16_mount(&g_fs);

    ASSERT_EQ(g_fs.fat_lba,      (uint32_t)1, "fat_lba = 1");
    ASSERT_EQ(g_fs.root_dir_lba, (uint32_t)2, "root_dir_lba = 2");
    ASSERT_EQ(g_fs.data_lba,     (uint32_t)3, "data_lba = 3");
}

static void test_mount_fails_bad_signature(void)
{
    setup_fs();
    /* Corrupt the boot signature */
    disk[0x1FE] = 0x00;
    int r = fat16_mount(&g_fs);
    ASSERT_EQ(r, -1, "fat16_mount fails when signature is missing");
    /* Restore */
    disk[0x1FE] = 0x55;
}

static int always_fail_read(void *ctx, uint32_t lba, void *buf)
{
    (void)ctx; (void)lba; (void)buf;
    return -1;
}

static void test_mount_fails_read_error(void)
{
    fat16_ctx_t fail_fs;
    fail_fs.sector_read = always_fail_read;
    fail_fs.ctx         = (void *)0;
    int r = fat16_mount(&fail_fs);
    ASSERT_EQ(r, -1, "fat16_mount fails when sector_read returns -1");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 2. fat16_vfs_ops.readdir
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_readdir_entry0(void)
{
    setup_fs();
    fat16_mount(&g_fs);

    vfs_dirent_t ent;
    int r = fat16_vfs_ops.readdir(&g_fs, "/", 0, &ent);
    ASSERT_EQ(r, 0,  "readdir[0] returns 0");
    ASSERT_STR_EQ(ent.name, "HELLO.TXT", "readdir[0] name is HELLO.TXT");
    ASSERT_EQ(ent.size, (uint32_t)13,    "readdir[0] size is 13");
    ASSERT_EQ(ent.type, (uint8_t)VFS_TYPE_FILE, "readdir[0] type is FILE");
}

static void test_readdir_entry1(void)
{
    setup_fs();
    fat16_mount(&g_fs);

    vfs_dirent_t ent;
    int r = fat16_vfs_ops.readdir(&g_fs, "/", 1, &ent);
    ASSERT_EQ(r, 0,  "readdir[1] returns 0");
    ASSERT_STR_EQ(ent.name, "WORLD.TXT", "readdir[1] name is WORLD.TXT");
    ASSERT_EQ(ent.size, (uint32_t)12,    "readdir[1] size is 12");
}

static void test_readdir_past_end(void)
{
    setup_fs();
    fat16_mount(&g_fs);

    vfs_dirent_t ent;
    int r = fat16_vfs_ops.readdir(&g_fs, "/", 2, &ent);
    ASSERT_EQ(r, -1, "readdir[2] returns -1 (end of directory)");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 3. fat16_vfs_ops.open
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_open_existing(void)
{
    setup_fs();
    fat16_mount(&g_fs);

    vfs_node_t node;
    int r = fat16_vfs_ops.open(&g_fs, "HELLO.TXT", &node);
    ASSERT_EQ(r, 0,              "open HELLO.TXT returns 0");
    ASSERT_EQ(node.inode,  2u,   "inode = cluster 2");
    ASSERT_EQ(node.size,  13u,   "size  = 13");
    ASSERT_EQ(node.type, (uint8_t)VFS_TYPE_FILE, "type  = FILE");
}

static void test_open_lowercase_path(void)
{
    /* fat16_open must uppercase the path before comparing */
    setup_fs();
    fat16_mount(&g_fs);

    vfs_node_t node;
    int r = fat16_vfs_ops.open(&g_fs, "hello.txt", &node);
    ASSERT_EQ(r, 0,    "open 'hello.txt' (lowercase) succeeds");
    ASSERT_EQ(node.size, 13u, "lowercase open returns correct size");
}

static void test_open_with_slash(void)
{
    setup_fs();
    fat16_mount(&g_fs);

    vfs_node_t node;
    int r = fat16_vfs_ops.open(&g_fs, "/WORLD.TXT", &node);
    ASSERT_EQ(r, 0, "open '/WORLD.TXT' (leading slash) succeeds");
    ASSERT_EQ(node.size, 12u, "WORLD.TXT size is 12");
}

static void test_open_missing_file(void)
{
    setup_fs();
    fat16_mount(&g_fs);

    vfs_node_t node;
    int r = fat16_vfs_ops.open(&g_fs, "NOFILE.TXT", &node);
    ASSERT_EQ(r, -1, "open non-existent file returns -1");
}

static void test_open_second_file(void)
{
    setup_fs();
    fat16_mount(&g_fs);

    vfs_node_t node;
    int r = fat16_vfs_ops.open(&g_fs, "WORLD.TXT", &node);
    ASSERT_EQ(r, 0,    "open WORLD.TXT returns 0");
    ASSERT_EQ(node.inode, 3u,  "WORLD.TXT inode = cluster 3");
    ASSERT_EQ(node.size, 12u,  "WORLD.TXT size  = 12");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 4. fat16_vfs_ops.read
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_read_full_file(void)
{
    setup_fs();
    fat16_mount(&g_fs);

    vfs_node_t node;
    fat16_vfs_ops.open(&g_fs, "HELLO.TXT", &node);

    uint8_t buf[32];
    int n = fat16_vfs_ops.read(&g_fs, &node, 0, 13, buf);
    ASSERT_EQ(n, 13, "read 13 bytes returns 13");

    buf[n] = '\0';
    ASSERT_STR_EQ((char *)buf, "Hello, World!", "content is 'Hello, World!'");
}

static void test_read_partial_from_offset(void)
{
    setup_fs();
    fat16_mount(&g_fs);

    vfs_node_t node;
    fat16_vfs_ops.open(&g_fs, "HELLO.TXT", &node);

    uint8_t buf[8];
    /* Read bytes 7..12 of "Hello, World!" → "World!" */
    int n = fat16_vfs_ops.read(&g_fs, &node, 7, 6, buf);
    ASSERT_EQ(n, 6, "partial read returns 6 bytes");
    buf[n] = '\0';
    ASSERT_STR_EQ((char *)buf, "World!", "partial read returns 'World!'");
}

static void test_read_second_file(void)
{
    setup_fs();
    fat16_mount(&g_fs);

    vfs_node_t node;
    fat16_vfs_ops.open(&g_fs, "WORLD.TXT", &node);

    uint8_t buf[32];
    int n = fat16_vfs_ops.read(&g_fs, &node, 0, 12, buf);
    ASSERT_EQ(n, 12, "read WORLD.TXT returns 12 bytes");
    buf[n] = '\0';
    ASSERT_STR_EQ((char *)buf, "Hello World!", "WORLD.TXT content matches");
}

static void test_read_zero_len(void)
{
    setup_fs();
    fat16_mount(&g_fs);

    vfs_node_t node;
    fat16_vfs_ops.open(&g_fs, "HELLO.TXT", &node);

    uint8_t buf[8];
    int n = fat16_vfs_ops.read(&g_fs, &node, 0, 0, buf);
    ASSERT_EQ(n, 0, "read with len=0 returns 0");
}

static void test_read_past_eof_offset(void)
{
    setup_fs();
    fat16_mount(&g_fs);

    vfs_node_t node;
    fat16_vfs_ops.open(&g_fs, "HELLO.TXT", &node);

    /* VFS layer clamps len before calling read, but the driver must also
       handle offset >= file size gracefully. */
    uint8_t buf[8];
    int n = fat16_vfs_ops.read(&g_fs, &node, 100, 8, buf);
    /* cluster 100 doesn't exist — expect 0 (no data) */
    ASSERT(n <= 0, "read at large offset returns <= 0");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 5. fat16_create
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_create_new_file(void)
{
    setup_write_fs();
    int r = fat16_vfs_ops.create(&g_fs, "TEST.TXT");
    ASSERT_EQ(r, 0, "create TEST.TXT returns 0");
}

static void test_create_duplicate_fails(void)
{
    setup_write_fs();
    fat16_vfs_ops.create(&g_fs, "TEST.TXT");
    int r = fat16_vfs_ops.create(&g_fs, "TEST.TXT");
    ASSERT_EQ(r, -1, "create duplicate name returns -1");
}

static void test_create_no_write_support(void)
{
    setup_write_fs();
    g_fs.sector_write = (void *)0;   /* simulate read-only mount */
    int r = fat16_vfs_ops.create(&g_fs, "NEWFILE.TXT");
    ASSERT_EQ(r, -1, "create returns -1 when sector_write is NULL");
}

static void test_create_appears_in_readdir(void)
{
    setup_write_fs();
    fat16_vfs_ops.create(&g_fs, "THIRD.TXT");

    vfs_dirent_t ent;
    int found = 0;
    for (uint32_t i = 0; fat16_vfs_ops.readdir(&g_fs, "/", i, &ent) == 0; i++) {
        if (fw_streq(ent.name, "THIRD.TXT")) { found = 1; break; }
    }
    ASSERT_EQ(found, 1, "created file appears in readdir");
}

static void test_create_existing_name_blocked(void)
{
    /* Existing files in the image must also be blocked. */
    setup_write_fs();
    int r = fat16_vfs_ops.create(&g_fs, "HELLO.TXT");
    ASSERT_EQ(r, -1, "create fails when name matches existing file");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 6. fat16_write
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_write_small_content(void)
{
    setup_write_fs();
    fat16_vfs_ops.create(&g_fs, "WR.TXT");

    vfs_node_t node;
    fat16_vfs_ops.open(&g_fs, "WR.TXT", &node);

    const uint8_t data[] = "hello";
    int n = fat16_vfs_ops.write(&g_fs, &node, 0, 5, data);
    ASSERT_EQ(n, 5, "write returns 5 for 5-byte input");
}

static void test_write_updates_file_size(void)
{
    setup_write_fs();
    fat16_vfs_ops.create(&g_fs, "SZ.TXT");

    vfs_node_t node;
    fat16_vfs_ops.open(&g_fs, "SZ.TXT", &node);
    ASSERT_EQ(node.size, 0u, "new file size is 0");

    const uint8_t data[] = "size test";
    fat16_vfs_ops.write(&g_fs, &node, 0, 9, data);

    /* Re-open to read the persisted directory entry */
    vfs_node_t node2;
    fat16_vfs_ops.open(&g_fs, "SZ.TXT", &node2);
    ASSERT_EQ(node2.size, 9u, "file size is 9 after writing 9 bytes");
}

static void test_write_read_roundtrip(void)
{
    setup_write_fs();
    fat16_vfs_ops.create(&g_fs, "RT.TXT");

    /* Write */
    vfs_node_t node;
    fat16_vfs_ops.open(&g_fs, "RT.TXT", &node);
    const uint8_t msg[] = "roundtrip";
    fat16_vfs_ops.write(&g_fs, &node, 0, 9, msg);

    /* Read back */
    vfs_node_t node2;
    fat16_vfs_ops.open(&g_fs, "RT.TXT", &node2);
    uint8_t buf[16];
    int n = fat16_vfs_ops.read(&g_fs, &node2, 0, node2.size, buf);
    ASSERT_EQ(n, 9, "read returns 9 bytes after write");
    buf[n] = '\0';
    ASSERT_STR_EQ((char *)buf, "roundtrip", "read-back content matches written data");
}

static void test_write_no_write_support(void)
{
    setup_write_fs();
    g_fs.sector_write = (void *)0;

    vfs_node_t node;
    fat16_vfs_ops.open(&g_fs, "HELLO.TXT", &node);
    const uint8_t d[] = "x";
    int n = fat16_vfs_ops.write(&g_fs, &node, 0, 1, d);
    ASSERT_EQ(n, -1, "write returns -1 when sector_write is NULL");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 7. fat16_remove
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_remove_existing_file(void)
{
    setup_write_fs();
    int r = fat16_vfs_ops.remove(&g_fs, "HELLO.TXT");
    ASSERT_EQ(r, 0, "remove HELLO.TXT returns 0");
}

static void test_remove_marks_entry_deleted(void)
{
    setup_write_fs();
    fat16_vfs_ops.remove(&g_fs, "HELLO.TXT");

    /* After removal, opening the file must fail. */
    vfs_node_t node;
    int r = fat16_vfs_ops.open(&g_fs, "HELLO.TXT", &node);
    ASSERT_EQ(r, -1, "open after remove returns -1");
}

static void test_remove_frees_fat_entry(void)
{
    setup_write_fs();
    fat16_vfs_ops.remove(&g_fs, "HELLO.TXT");

    /* FAT entry 2 (HELLO.TXT's cluster) must now be 0x0000. */
    uint8_t *fat = disk + 1 * 512;
    uint16_t entry2 = (uint16_t)(fat[4] | ((uint16_t)fat[5] << 8));
    ASSERT_EQ(entry2, (uint16_t)0x0000, "FAT entry for removed file is freed (0x0000)");
}

static void test_remove_nonexistent_fails(void)
{
    setup_write_fs();
    int r = fat16_vfs_ops.remove(&g_fs, "NOFILE.TXT");
    ASSERT_EQ(r, -1, "remove non-existent file returns -1");
}

static void test_remove_no_write_support(void)
{
    setup_write_fs();
    g_fs.sector_write = (void *)0;
    int r = fat16_vfs_ops.remove(&g_fs, "HELLO.TXT");
    ASSERT_EQ(r, -1, "remove returns -1 when sector_write is NULL");
}

static void test_remove_then_create_reuses_slot(void)
{
    setup_write_fs();
    fat16_vfs_ops.remove(&g_fs, "HELLO.TXT");

    /* Creating a new file should succeed (free slot exists). */
    int r = fat16_vfs_ops.create(&g_fs, "NEWFILE.TXT");
    ASSERT_EQ(r, 0, "create succeeds after remove (slot reused)");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    build_image();

    /* fat16_mount */
    RUN_SUITE(test_mount_succeeds);
    RUN_SUITE(test_mount_parses_bpb);
    RUN_SUITE(test_mount_derives_lba_addresses);
    RUN_SUITE(test_mount_fails_bad_signature);
    RUN_SUITE(test_mount_fails_read_error);

    /* readdir */
    RUN_SUITE(test_readdir_entry0);
    RUN_SUITE(test_readdir_entry1);
    RUN_SUITE(test_readdir_past_end);

    /* open */
    RUN_SUITE(test_open_existing);
    RUN_SUITE(test_open_lowercase_path);
    RUN_SUITE(test_open_with_slash);
    RUN_SUITE(test_open_missing_file);
    RUN_SUITE(test_open_second_file);

    /* read */
    RUN_SUITE(test_read_full_file);
    RUN_SUITE(test_read_partial_from_offset);
    RUN_SUITE(test_read_second_file);
    RUN_SUITE(test_read_zero_len);
    RUN_SUITE(test_read_past_eof_offset);

    /* create */
    RUN_SUITE(test_create_new_file);
    RUN_SUITE(test_create_duplicate_fails);
    RUN_SUITE(test_create_no_write_support);
    RUN_SUITE(test_create_appears_in_readdir);
    RUN_SUITE(test_create_existing_name_blocked);

    /* write */
    RUN_SUITE(test_write_small_content);
    RUN_SUITE(test_write_updates_file_size);
    RUN_SUITE(test_write_read_roundtrip);
    RUN_SUITE(test_write_no_write_support);

    /* remove */
    RUN_SUITE(test_remove_existing_file);
    RUN_SUITE(test_remove_marks_entry_deleted);
    RUN_SUITE(test_remove_frees_fat_entry);
    RUN_SUITE(test_remove_nonexistent_fails);
    RUN_SUITE(test_remove_no_write_support);
    RUN_SUITE(test_remove_then_create_reuses_slot);

    TEST_SUMMARY();
}
