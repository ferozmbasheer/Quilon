/*
 * Quilon OS -- VFS Unit Tests
 *
 * Tests the VFS layer (kernel/kernel/vfs.c) using a mock filesystem driver
 * backed by static data.  No hardware, no ATA, no FAT16 -- the mock driver
 * returns hand-crafted responses for a tiny two-file "filesystem".
 *
 * Build & run:  cd tests && make
 */

#include "framework.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/vfs.h>

/* -- Mock filesystem --------------------------------------------------------
 *
 * Virtual layout:
 *   README.TXT   "Hello, VFS!"  (11 bytes)
 *   NOTES.TXT    "Test note."   (10 bytes)
 * --------------------------------------------------------------------------- */

static const char mock_readme[] = "Hello, VFS!";
static const char mock_notes[]  = "Test note.";

typedef struct { const char *name; const char *data; uint32_t size; } mock_file_t;
static const mock_file_t mock_files[] = {
    { "README.TXT", mock_readme, sizeof(mock_readme) - 1 },
    { "NOTES.TXT",  mock_notes,  sizeof(mock_notes)  - 1 },
};
#define MOCK_FILE_COUNT 2

/* -- Driver functions ------------------------------------------------------- */

static int mock_open(void *ctx, const char *path, vfs_node_t *out)
{
    (void)ctx;
    if (*path == '/') path++;   /* strip leading slash */

    for (int i = 0; i < MOCK_FILE_COUNT; i++) {
        if (fw_streq(path, mock_files[i].name)) {
            out->inode  = (uint32_t)i;
            out->size   = mock_files[i].size;
            out->type   = VFS_TYPE_FILE;
            out->in_use = 1;
            out->offset = 0;
            return 0;
        }
    }
    return -1;
}

static int mock_read(void *ctx, vfs_node_t *node, uint32_t offset,
                     uint32_t size, uint8_t *buf)
{
    (void)ctx;
    int idx = (int)node->inode;
    if (idx < 0 || idx >= MOCK_FILE_COUNT) return -1;
    if (offset >= mock_files[idx].size)    return 0;

    uint32_t avail = mock_files[idx].size - offset;
    if (size > avail) size = avail;
    memcpy(buf, mock_files[idx].data + offset, size);
    return (int)size;
}

static int mock_readdir(void *ctx, const char *path, uint32_t index,
                         vfs_dirent_t *out)
{
    (void)ctx; (void)path;   /* mock is a flat filesystem */
    if (index >= MOCK_FILE_COUNT) return -1;

    /* Use fw_streq's cousin to copy the name */
    const char *src = mock_files[index].name;
    int i = 0;
    while (*src && i < VFS_NAME_MAX) out->name[i++] = *src++;
    out->name[i] = '\0';

    out->size = mock_files[index].size;
    out->type = VFS_TYPE_FILE;
    return 0;
}

static void mock_close(void *ctx, vfs_node_t *node)
{
    (void)ctx; (void)node;
}

static const vfs_ops_t mock_ops = {
    .open    = mock_open,
    .read    = mock_read,
    .readdir = mock_readdir,
    .close   = mock_close,
    /* write / create / remove intentionally absent (NULL) */
};

/* -- Write-capable mock ---------------------------------------------------- */

static uint8_t  mock_wbuf[256];
static uint32_t mock_wbuf_len;

static int mock_write_fn(void *ctx, vfs_node_t *node, uint32_t offset,
                          uint32_t size, const uint8_t *buf)
{
    (void)ctx;
    if (!buf || size == 0) return 0;
    if (offset + size > sizeof(mock_wbuf)) return -1;
    int i;
    for (i = 0; i < (int)size; i++) mock_wbuf[offset + i] = buf[i];
    if (offset + size > mock_wbuf_len) {
        mock_wbuf_len = offset + size;
        node->size    = mock_wbuf_len;
    }
    return (int)size;
}

static int mock_create_fn(void *ctx, const char *path)
{
    (void)ctx; (void)path;
    return 0;   /* always succeed in mock */
}

static int mock_remove_fn(void *ctx, const char *path)
{
    (void)ctx;
    const char *name = (*path == '/') ? path + 1 : path;
    return (fw_streq(name, "README.TXT") || fw_streq(name, "NOTES.TXT")) ? 0 : -1;
}

static const vfs_ops_t mock_rw_ops = {
    .open    = mock_open,
    .read    = mock_read,
    .write   = mock_write_fn,
    .readdir = mock_readdir,
    .close   = mock_close,
    .create  = mock_create_fn,
    .remove  = mock_remove_fn,
};

/* -- Section-12.1 mock: stat / mkdir / rename ------------------------------ */

static int mock_stat_fn(void *ctx, const char *path, vfs_stat_t *out)
{
    (void)ctx;
    if (*path == '/') path++;
    if (*path == '\0') {   /* root "/" */
        out->size = 0;
        out->type = VFS_TYPE_DIR;
        return 0;
    }
    for (int i = 0; i < MOCK_FILE_COUNT; i++) {
        if (fw_streq(path, mock_files[i].name)) {
            out->size = mock_files[i].size;
            out->type = VFS_TYPE_FILE;
            return 0;
        }
    }
    if (fw_streq(path, "TESTDIR")) {
        out->size = 0;
        out->type = VFS_TYPE_DIR;
        return 0;
    }
    return -1;
}

static int mock_mkdir_fn(void *ctx, const char *path)
{
    (void)ctx; (void)path;
    return 0;
}

static int mock_rename_fn(void *ctx, const char *oldpath, const char *newpath)
{
    (void)ctx; (void)newpath;
    const char *name = (*oldpath == '/') ? oldpath + 1 : oldpath;
    return (fw_streq(name, "README.TXT") || fw_streq(name, "NOTES.TXT")) ? 0 : -1;
}

static const vfs_ops_t mock_full_ops = {
    .open    = mock_open,
    .read    = mock_read,
    .write   = mock_write_fn,
    .readdir = mock_readdir,
    .close   = mock_close,
    .create  = mock_create_fn,
    .remove  = mock_remove_fn,
    .stat    = mock_stat_fn,
    .mkdir   = mock_mkdir_fn,
    .rename  = mock_rename_fn,
};

/* ===========================================================================
 * 1. vfs_mount / vfs_mounted
 * =========================================================================== */

static void test_not_mounted_initially(void)
{
    /* vfs.c starts with mounted_ops = NULL -- first access should report not
       mounted.  (But a previous test may have called vfs_mount -- we reset
       between suites by mounting and relying on the test order.)          */
    ASSERT_EQ(vfs_mounted(), 0, "vfs_mounted() returns 0 before any mount");
}

static void test_mounted_after_mount(void)
{
    vfs_mount(&mock_ops, (void *)0);
    ASSERT_EQ(vfs_mounted(), 1, "vfs_mounted() returns 1 after vfs_mount()");
}

/* ===========================================================================
 * 2. vfs_open
 * =========================================================================== */

static void test_open_existing_file(void)
{
    vfs_mount(&mock_ops, (void *)0);
    int fd = vfs_open("README.TXT");
    ASSERT(fd >= VFS_FD_BASE, "vfs_open returns fd >= VFS_FD_BASE for known file");
    vfs_close(fd);
}

static void test_open_with_leading_slash(void)
{
    vfs_mount(&mock_ops, (void *)0);
    int fd = vfs_open("/README.TXT");
    ASSERT(fd >= VFS_FD_BASE, "vfs_open strips leading '/' and finds file");
    vfs_close(fd);
}

static void test_open_missing_file(void)
{
    vfs_mount(&mock_ops, (void *)0);
    int fd = vfs_open("NOFILE.TXT");
    ASSERT_EQ(fd, -1, "vfs_open returns -1 for non-existent file");
}

static void test_open_when_not_mounted(void)
{
    /* Force unmounted state by providing a NULL ops pointer */
    vfs_mount((const vfs_ops_t *)0, (void *)0);
    int fd = vfs_open("README.TXT");
    ASSERT_EQ(fd, -1, "vfs_open returns -1 when no filesystem is mounted");
    /* Re-mount for subsequent suites */
    vfs_mount(&mock_ops, (void *)0);
}

static void test_open_two_distinct_fds(void)
{
    vfs_mount(&mock_ops, (void *)0);
    int fd1 = vfs_open("README.TXT");
    int fd2 = vfs_open("NOTES.TXT");
    ASSERT(fd1 >= VFS_FD_BASE, "first  open returns valid fd");
    ASSERT(fd2 >= VFS_FD_BASE, "second open returns valid fd");
    ASSERT(fd1 != fd2,         "two opens return distinct fds");
    vfs_close(fd1);
    vfs_close(fd2);
}

/* ===========================================================================
 * 3. vfs_read
 * =========================================================================== */

static void test_read_full_file(void)
{
    vfs_mount(&mock_ops, (void *)0);
    int fd = vfs_open("README.TXT");
    ASSERT(fd >= 0, "open README.TXT");

    char buf[32];
    int n = vfs_read(fd, buf, sizeof(buf));
    ASSERT_EQ(n, (int)(sizeof(mock_readme) - 1), "read returns exact file length");
    buf[n] = '\0';
    ASSERT_STR_EQ(buf, mock_readme, "read content matches mock_readme");
    vfs_close(fd);
}

static void test_read_advances_offset(void)
{
    vfs_mount(&mock_ops, (void *)0);
    int fd = vfs_open("README.TXT");

    char buf[4];
    int n1 = vfs_read(fd, buf, 4);
    ASSERT_EQ(n1, 4, "first partial read returns 4 bytes");

    char buf2[32];
    int n2 = vfs_read(fd, buf2, sizeof(buf2));
    /* Remaining bytes after reading 4 from "Hello, VFS!" (11 bytes) = 7 */
    ASSERT_EQ(n2, (int)(sizeof(mock_readme) - 1) - 4,
              "second read returns remaining bytes");
    vfs_close(fd);
}

static void test_read_at_eof_returns_zero(void)
{
    vfs_mount(&mock_ops, (void *)0);
    int fd = vfs_open("NOTES.TXT");

    char buf[64];
    vfs_read(fd, buf, sizeof(buf));   /* drain the file */
    int n = vfs_read(fd, buf, sizeof(buf));
    ASSERT_EQ(n, 0, "read at EOF returns 0");
    vfs_close(fd);
}

static void test_read_invalid_fd(void)
{
    vfs_mount(&mock_ops, (void *)0);
    char buf[8];
    int n = vfs_read(99, buf, sizeof(buf));
    ASSERT_EQ(n, -1, "read with out-of-range fd returns -1");
}

static void test_read_closed_fd(void)
{
    vfs_mount(&mock_ops, (void *)0);
    int fd = vfs_open("README.TXT");
    vfs_close(fd);
    char buf[8];
    int n = vfs_read(fd, buf, sizeof(buf));
    ASSERT_EQ(n, -1, "read after close returns -1");
}

/* ===========================================================================
 * 4. vfs_close
 * =========================================================================== */

static void test_close_valid_fd(void)
{
    vfs_mount(&mock_ops, (void *)0);
    int fd = vfs_open("README.TXT");
    int r = vfs_close(fd);
    ASSERT_EQ(r, 0, "close returns 0 for a valid fd");
}

static void test_close_invalid_fd(void)
{
    vfs_mount(&mock_ops, (void *)0);
    int r = vfs_close(99);
    ASSERT_EQ(r, -1, "close returns -1 for an invalid fd");
}

static void test_reopen_after_close(void)
{
    vfs_mount(&mock_ops, (void *)0);
    int fd1 = vfs_open("README.TXT");
    vfs_close(fd1);
    int fd2 = vfs_open("README.TXT");
    ASSERT(fd2 >= VFS_FD_BASE, "file can be re-opened after close");
    ASSERT_EQ(fd1, fd2, "re-open reuses the freed fd slot");
    vfs_close(fd2);
}

/* ===========================================================================
 * 5. vfs_readdir
 * =========================================================================== */

static void test_readdir_first_entry(void)
{
    vfs_mount(&mock_ops, (void *)0);
    vfs_dirent_t ent;
    int r = vfs_readdir(0, &ent);
    ASSERT_EQ(r, 0,             "readdir index 0 returns 0");
    ASSERT_STR_EQ(ent.name, "README.TXT", "readdir[0] name is README.TXT");
    ASSERT_EQ(ent.size, (uint32_t)(sizeof(mock_readme) - 1),
              "readdir[0] size matches mock_readme length");
    ASSERT_EQ(ent.type, (uint8_t)VFS_TYPE_FILE, "readdir[0] type is FILE");
}

static void test_readdir_second_entry(void)
{
    vfs_mount(&mock_ops, (void *)0);
    vfs_dirent_t ent;
    int r = vfs_readdir(1, &ent);
    ASSERT_EQ(r, 0,            "readdir index 1 returns 0");
    ASSERT_STR_EQ(ent.name, "NOTES.TXT", "readdir[1] name is NOTES.TXT");
}

static void test_readdir_past_end(void)
{
    vfs_mount(&mock_ops, (void *)0);
    vfs_dirent_t ent;
    int r = vfs_readdir(MOCK_FILE_COUNT, &ent);
    ASSERT_EQ(r, -1, "readdir past last entry returns -1");
}

static void test_readdir_not_mounted(void)
{
    vfs_mount((const vfs_ops_t *)0, (void *)0);
    vfs_dirent_t ent;
    int r = vfs_readdir(0, &ent);
    ASSERT_EQ(r, -1, "readdir returns -1 when not mounted");
    vfs_mount(&mock_ops, (void *)0);
}

/* ===========================================================================
 * 6. vfs_write
 * =========================================================================== */

static void test_write_basic(void)
{
    mock_wbuf_len = 0;
    vfs_mount(&mock_rw_ops, (void *)0);
    int fd = vfs_open("README.TXT");
    ASSERT(fd >= VFS_FD_BASE, "open for write returns valid fd");

    const char msg[] = "Updated";
    int n = vfs_write(fd, msg, (uint32_t)(sizeof(msg) - 1));
    ASSERT_EQ(n, (int)(sizeof(msg) - 1), "write returns bytes written");
    vfs_close(fd);
}

static void test_write_advances_offset(void)
{
    mock_wbuf_len = 0;
    int i;
    for (i = 0; i < (int)sizeof(mock_wbuf); i++) mock_wbuf[i] = 0;

    vfs_mount(&mock_rw_ops, (void *)0);
    int fd = vfs_open("README.TXT");

    vfs_write(fd, (const uint8_t *)"AB", 2);
    vfs_write(fd, (const uint8_t *)"CD", 2);

    ASSERT_EQ((char)mock_wbuf[0], 'A', "byte 0 after sequential writes = 'A'");
    ASSERT_EQ((char)mock_wbuf[1], 'B', "byte 1 after sequential writes = 'B'");
    ASSERT_EQ((char)mock_wbuf[2], 'C', "byte 2 after sequential writes = 'C'");
    ASSERT_EQ((char)mock_wbuf[3], 'D', "byte 3 after sequential writes = 'D'");
    vfs_close(fd);
}

static void test_write_no_driver_support(void)
{
    vfs_mount(&mock_ops, (void *)0);   /* mock_ops has no .write */
    int fd = vfs_open("README.TXT");
    int n = vfs_write(fd, (const uint8_t *)"test", 4);
    ASSERT_EQ(n, -1, "write returns -1 when driver has no write support");
    vfs_close(fd);
}

static void test_write_invalid_fd(void)
{
    vfs_mount(&mock_rw_ops, (void *)0);
    int n = vfs_write(99, (const uint8_t *)"test", 4);
    ASSERT_EQ(n, -1, "write with invalid fd returns -1");
}

static void test_write_not_mounted(void)
{
    vfs_mount((const vfs_ops_t *)0, (void *)0);
    int n = vfs_write(VFS_FD_BASE, (const uint8_t *)"x", 1);
    ASSERT_EQ(n, -1, "write returns -1 when not mounted");
    vfs_mount(&mock_rw_ops, (void *)0);
}

/* ===========================================================================
 * 7. vfs_create / vfs_remove
 * =========================================================================== */

static void test_create_succeeds(void)
{
    vfs_mount(&mock_rw_ops, (void *)0);
    int r = vfs_create("NEWFILE.TXT");
    ASSERT_EQ(r, 0, "vfs_create returns 0 on success");
}

static void test_create_no_driver_support(void)
{
    vfs_mount(&mock_ops, (void *)0);   /* mock_ops has no .create */
    int r = vfs_create("NEWFILE.TXT");
    ASSERT_EQ(r, -1, "vfs_create returns -1 when driver has no create support");
}

static void test_create_not_mounted(void)
{
    vfs_mount((const vfs_ops_t *)0, (void *)0);
    int r = vfs_create("X.TXT");
    ASSERT_EQ(r, -1, "vfs_create returns -1 when not mounted");
    vfs_mount(&mock_rw_ops, (void *)0);
}

static void test_remove_succeeds(void)
{
    vfs_mount(&mock_rw_ops, (void *)0);
    int r = vfs_remove("README.TXT");
    ASSERT_EQ(r, 0, "vfs_remove returns 0 for existing file");
}

static void test_remove_fails_for_missing(void)
{
    vfs_mount(&mock_rw_ops, (void *)0);
    int r = vfs_remove("NONEXISTENT.TXT");
    ASSERT_EQ(r, -1, "vfs_remove returns -1 for non-existent file");
}

static void test_remove_no_driver_support(void)
{
    vfs_mount(&mock_ops, (void *)0);   /* mock_ops has no .remove */
    int r = vfs_remove("README.TXT");
    ASSERT_EQ(r, -1, "vfs_remove returns -1 when driver has no remove support");
}

static void test_remove_not_mounted(void)
{
    vfs_mount((const vfs_ops_t *)0, (void *)0);
    int r = vfs_remove("README.TXT");
    ASSERT_EQ(r, -1, "vfs_remove returns -1 when not mounted");
    vfs_mount(&mock_rw_ops, (void *)0);
}

/* ===========================================================================
 * 8. fd table limits
 * =========================================================================== */

static void test_fd_table_full(void)
{
    vfs_mount(&mock_ops, (void *)0);
    int fds[VFS_MAX_FDS];
    int opened = 0;

    /* Open VFS_MAX_FDS files to fill the table */
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        fds[i] = vfs_open("README.TXT");
        if (fds[i] >= 0) opened++;
    }
    ASSERT_EQ(opened, VFS_MAX_FDS, "can open VFS_MAX_FDS files simultaneously");

    /* One more open should fail */
    int overflow = vfs_open("README.TXT");
    ASSERT_EQ(overflow, -1, "opening beyond VFS_MAX_FDS returns -1");

    /* Close all fds */
    for (int i = 0; i < VFS_MAX_FDS; i++)
        if (fds[i] >= 0) vfs_close(fds[i]);
}

/* ===========================================================================
 * 9. vfs_lseek  (section 12.1)
 * =========================================================================== */

static void test_lseek_set(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    int fd = vfs_open("README.TXT");  /* size = 11 */
    ASSERT(fd >= VFS_FD_BASE, "open README.TXT for lseek test");

    int pos = vfs_lseek(fd, 4, VFS_SEEK_SET);
    ASSERT_EQ(pos, 4, "SEEK_SET to 4 returns 4");

    char buf[8];
    int n = vfs_read(fd, buf, 4);
    ASSERT_EQ(n, 4, "read 4 bytes from offset 4");
    buf[4] = '\0';
    /* "Hello, VFS!" offset 4 = "o, V" */
    ASSERT_STR_EQ(buf, "o, V", "read after SEEK_SET returns correct bytes");
    vfs_close(fd);
}

static void test_lseek_set_zero(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    int fd = vfs_open("README.TXT");

    /* Read a few bytes, then rewind. */
    char tmp[4];
    vfs_read(fd, tmp, 4);

    int pos = vfs_lseek(fd, 0, VFS_SEEK_SET);
    ASSERT_EQ(pos, 0, "SEEK_SET 0 rewinds to beginning");

    char buf[8];
    int n = vfs_read(fd, buf, 5);
    buf[n] = '\0';
    ASSERT_STR_EQ(buf, "Hello", "read from rewound position returns start of file");
    vfs_close(fd);
}

static void test_lseek_cur_forward(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    int fd = vfs_open("README.TXT");

    vfs_lseek(fd, 2, VFS_SEEK_SET);          /* pos = 2 */
    int pos = vfs_lseek(fd, 3, VFS_SEEK_CUR); /* pos = 5 */
    ASSERT_EQ(pos, 5, "SEEK_CUR +3 from offset 2 = 5");
    vfs_close(fd);
}

static void test_lseek_cur_backward(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    int fd = vfs_open("README.TXT");

    vfs_lseek(fd, 6, VFS_SEEK_SET);
    int pos = vfs_lseek(fd, -4, VFS_SEEK_CUR);
    ASSERT_EQ(pos, 2, "SEEK_CUR -4 from offset 6 = 2");
    vfs_close(fd);
}

static void test_lseek_end(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    int fd = vfs_open("README.TXT");   /* size = 11 */

    int pos = vfs_lseek(fd, -3, VFS_SEEK_END);
    ASSERT_EQ(pos, 8, "SEEK_END -3 on 11-byte file = 8");

    char buf[4];
    int n = vfs_read(fd, buf, 3);
    ASSERT_EQ(n, 3, "read 3 bytes from offset 8");
    buf[3] = '\0';
    ASSERT_STR_EQ(buf, "FS!", "last 3 bytes of 'Hello, VFS!'");
    vfs_close(fd);
}

static void test_lseek_invalid_fd(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    int pos = vfs_lseek(99, 0, VFS_SEEK_SET);
    ASSERT_EQ(pos, -1, "lseek with invalid fd returns -1");
}

static void test_lseek_negative_set(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    int fd = vfs_open("README.TXT");
    int pos = vfs_lseek(fd, -1, VFS_SEEK_SET);
    ASSERT_EQ(pos, -1, "SEEK_SET with negative offset returns -1");
    vfs_close(fd);
}

/* ===========================================================================
 * 10. vfs_stat  (section 12.1)
 * =========================================================================== */

static void test_stat_known_file(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    vfs_stat_t st;
    int r = vfs_stat("README.TXT", &st);
    ASSERT_EQ(r, 0, "stat of README.TXT returns 0");
    ASSERT_EQ(st.type, (uint8_t)VFS_TYPE_FILE, "README.TXT type is FILE");
    ASSERT_EQ(st.size, (uint32_t)(sizeof(mock_readme) - 1), "README.TXT size matches");
}

static void test_stat_root_dir(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    vfs_stat_t st;
    int r = vfs_stat("/", &st);
    ASSERT_EQ(r, 0, "stat of '/' returns 0");
    ASSERT_EQ(st.type, (uint8_t)VFS_TYPE_DIR, "'/' type is DIR");
}

static void test_stat_mock_directory(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    vfs_stat_t st;
    int r = vfs_stat("TESTDIR", &st);
    ASSERT_EQ(r, 0, "stat of TESTDIR returns 0");
    ASSERT_EQ(st.type, (uint8_t)VFS_TYPE_DIR, "TESTDIR type is DIR");
}

static void test_stat_unknown_path(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    vfs_stat_t st;
    int r = vfs_stat("GHOST.TXT", &st);
    ASSERT_EQ(r, -1, "stat of unknown path returns -1");
}

static void test_stat_not_mounted(void)
{
    vfs_mount((const vfs_ops_t *)0, (void *)0);
    vfs_stat_t st;
    int r = vfs_stat("README.TXT", &st);
    ASSERT_EQ(r, -1, "stat returns -1 when not mounted");
    vfs_mount(&mock_full_ops, (void *)0);
}

static void test_stat_no_driver_support(void)
{
    vfs_mount(&mock_ops, (void *)0);   /* mock_ops has no .stat */
    vfs_stat_t st;
    int r = vfs_stat("README.TXT", &st);
    ASSERT_EQ(r, -1, "stat returns -1 when driver has no stat support");
    vfs_mount(&mock_full_ops, (void *)0);
}

/* ===========================================================================
 * 11. vfs_mkdir  (section 12.1)
 * =========================================================================== */

static void test_mkdir_succeeds(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    int r = vfs_mkdir("NEWDIR");
    ASSERT_EQ(r, 0, "vfs_mkdir returns 0 on success");
}

static void test_mkdir_not_mounted(void)
{
    vfs_mount((const vfs_ops_t *)0, (void *)0);
    int r = vfs_mkdir("NEWDIR");
    ASSERT_EQ(r, -1, "vfs_mkdir returns -1 when not mounted");
    vfs_mount(&mock_full_ops, (void *)0);
}

static void test_mkdir_no_driver_support(void)
{
    vfs_mount(&mock_ops, (void *)0);
    int r = vfs_mkdir("NEWDIR");
    ASSERT_EQ(r, -1, "vfs_mkdir returns -1 when driver has no mkdir support");
    vfs_mount(&mock_full_ops, (void *)0);
}

/* ===========================================================================
 * 12. vfs_rename  (section 12.1)
 * =========================================================================== */

static void test_rename_succeeds(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    int r = vfs_rename("README.TXT", "READNEW.TXT");
    ASSERT_EQ(r, 0, "vfs_rename returns 0 for known old path");
}

static void test_rename_fails_unknown_old(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    int r = vfs_rename("GHOST.TXT", "OTHER.TXT");
    ASSERT_EQ(r, -1, "vfs_rename returns -1 for unknown old path");
}

static void test_rename_not_mounted(void)
{
    vfs_mount((const vfs_ops_t *)0, (void *)0);
    int r = vfs_rename("README.TXT", "OTHER.TXT");
    ASSERT_EQ(r, -1, "vfs_rename returns -1 when not mounted");
    vfs_mount(&mock_full_ops, (void *)0);
}

static void test_rename_no_driver_support(void)
{
    vfs_mount(&mock_ops, (void *)0);
    int r = vfs_rename("README.TXT", "OTHER.TXT");
    ASSERT_EQ(r, -1, "vfs_rename returns -1 when driver has no rename support");
    vfs_mount(&mock_full_ops, (void *)0);
}

/* ===========================================================================
 * 13. vfs_chdir / vfs_getcwd  (section 12.1)
 * =========================================================================== */

static void test_getcwd_initial(void)
{
    /* Reset to root by calling chdir -- tests may run in any order. */
    vfs_mount(&mock_full_ops, (void *)0);
    vfs_chdir("/");

    char buf[64];
    int r = vfs_getcwd(buf, sizeof(buf));
    ASSERT_EQ(r, 0, "vfs_getcwd returns 0");
    ASSERT_STR_EQ(buf, "/", "initial CWD is '/'");
}

static void test_chdir_root_always_succeeds(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    int r = vfs_chdir("/");
    ASSERT_EQ(r, 0, "chdir('/') always returns 0");

    char buf[64];
    vfs_getcwd(buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "/", "CWD is '/' after chdir('/')");
}

static void test_chdir_to_directory(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    vfs_chdir("/");   /* reset */

    int r = vfs_chdir("TESTDIR");
    ASSERT_EQ(r, 0, "chdir to known directory returns 0");

    char buf[64];
    vfs_getcwd(buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "TESTDIR", "CWD updated to TESTDIR");

    vfs_chdir("/");   /* reset for other tests */
}

static void test_chdir_to_file_fails(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    vfs_chdir("/");

    int r = vfs_chdir("README.TXT");
    ASSERT_EQ(r, -1, "chdir to a regular file returns -1");
}

static void test_chdir_to_unknown_path_fails(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    int r = vfs_chdir("NOSUCHDIR");
    ASSERT_EQ(r, -1, "chdir to unknown path returns -1");
}

static void test_getcwd_null_buf(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    int r = vfs_getcwd((char *)0, 64);
    ASSERT_EQ(r, -1, "vfs_getcwd with NULL buf returns -1");
}

static void test_getcwd_zero_len(void)
{
    vfs_mount(&mock_full_ops, (void *)0);
    char buf[4];
    int r = vfs_getcwd(buf, 0);
    ASSERT_EQ(r, -1, "vfs_getcwd with len=0 returns -1");
}

/* ===========================================================================
 * main
 * =========================================================================== */

int main(void)
{
    /* vfs_mounted / vfs_mount */
    RUN_SUITE(test_not_mounted_initially);
    RUN_SUITE(test_mounted_after_mount);

    /* vfs_open */
    RUN_SUITE(test_open_existing_file);
    RUN_SUITE(test_open_with_leading_slash);
    RUN_SUITE(test_open_missing_file);
    RUN_SUITE(test_open_when_not_mounted);
    RUN_SUITE(test_open_two_distinct_fds);

    /* vfs_read */
    RUN_SUITE(test_read_full_file);
    RUN_SUITE(test_read_advances_offset);
    RUN_SUITE(test_read_at_eof_returns_zero);
    RUN_SUITE(test_read_invalid_fd);
    RUN_SUITE(test_read_closed_fd);

    /* vfs_close */
    RUN_SUITE(test_close_valid_fd);
    RUN_SUITE(test_close_invalid_fd);
    RUN_SUITE(test_reopen_after_close);

    /* vfs_readdir */
    RUN_SUITE(test_readdir_first_entry);
    RUN_SUITE(test_readdir_second_entry);
    RUN_SUITE(test_readdir_past_end);
    RUN_SUITE(test_readdir_not_mounted);

    /* vfs_write */
    RUN_SUITE(test_write_basic);
    RUN_SUITE(test_write_advances_offset);
    RUN_SUITE(test_write_no_driver_support);
    RUN_SUITE(test_write_invalid_fd);
    RUN_SUITE(test_write_not_mounted);

    /* vfs_create / vfs_remove */
    RUN_SUITE(test_create_succeeds);
    RUN_SUITE(test_create_no_driver_support);
    RUN_SUITE(test_create_not_mounted);
    RUN_SUITE(test_remove_succeeds);
    RUN_SUITE(test_remove_fails_for_missing);
    RUN_SUITE(test_remove_no_driver_support);
    RUN_SUITE(test_remove_not_mounted);

    /* fd table limits */
    RUN_SUITE(test_fd_table_full);

    /* vfs_lseek (section 12.1) */
    RUN_SUITE(test_lseek_set);
    RUN_SUITE(test_lseek_set_zero);
    RUN_SUITE(test_lseek_cur_forward);
    RUN_SUITE(test_lseek_cur_backward);
    RUN_SUITE(test_lseek_end);
    RUN_SUITE(test_lseek_invalid_fd);
    RUN_SUITE(test_lseek_negative_set);

    /* vfs_stat (section 12.1) */
    RUN_SUITE(test_stat_known_file);
    RUN_SUITE(test_stat_root_dir);
    RUN_SUITE(test_stat_mock_directory);
    RUN_SUITE(test_stat_unknown_path);
    RUN_SUITE(test_stat_not_mounted);
    RUN_SUITE(test_stat_no_driver_support);

    /* vfs_mkdir (section 12.1) */
    RUN_SUITE(test_mkdir_succeeds);
    RUN_SUITE(test_mkdir_not_mounted);
    RUN_SUITE(test_mkdir_no_driver_support);

    /* vfs_rename (section 12.1) */
    RUN_SUITE(test_rename_succeeds);
    RUN_SUITE(test_rename_fails_unknown_old);
    RUN_SUITE(test_rename_not_mounted);
    RUN_SUITE(test_rename_no_driver_support);

    /* vfs_chdir / vfs_getcwd (section 12.1) */
    RUN_SUITE(test_getcwd_initial);
    RUN_SUITE(test_chdir_root_always_succeeds);
    RUN_SUITE(test_chdir_to_directory);
    RUN_SUITE(test_chdir_to_file_fails);
    RUN_SUITE(test_chdir_to_unknown_path_fails);
    RUN_SUITE(test_getcwd_null_buf);
    RUN_SUITE(test_getcwd_zero_len);

    TEST_SUMMARY();
}
