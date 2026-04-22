/*
 * Quilon OS — VFS Unit Tests
 *
 * Tests the VFS layer (kernel/kernel/vfs.c) using a mock filesystem driver
 * backed by static data.  No hardware, no ATA, no FAT16 — the mock driver
 * returns hand-crafted responses for a tiny two-file "filesystem".
 *
 * Build & run:  cd tests && make
 */

#include "framework.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/vfs.h>

/* ── Mock filesystem ────────────────────────────────────────────────────────
 *
 * Virtual layout:
 *   README.TXT   "Hello, VFS!"  (11 bytes)
 *   NOTES.TXT    "Test note."   (10 bytes)
 * ─────────────────────────────────────────────────────────────────────────── */

static const char mock_readme[] = "Hello, VFS!";
static const char mock_notes[]  = "Test note.";

typedef struct { const char *name; const char *data; uint32_t size; } mock_file_t;
static const mock_file_t mock_files[] = {
    { "README.TXT", mock_readme, sizeof(mock_readme) - 1 },
    { "NOTES.TXT",  mock_notes,  sizeof(mock_notes)  - 1 },
};
#define MOCK_FILE_COUNT 2

/* ── Driver functions ─────────────────────────────────────────────────────── */

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

static int mock_readdir(void *ctx, uint32_t index, vfs_dirent_t *out)
{
    (void)ctx;
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
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 1. vfs_mount / vfs_mounted
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_not_mounted_initially(void)
{
    /* vfs.c starts with mounted_ops = NULL — first access should report not
       mounted.  (But a previous test may have called vfs_mount — we reset
       between suites by mounting and relying on the test order.)          */
    ASSERT_EQ(vfs_mounted(), 0, "vfs_mounted() returns 0 before any mount");
}

static void test_mounted_after_mount(void)
{
    vfs_mount(&mock_ops, (void *)0);
    ASSERT_EQ(vfs_mounted(), 1, "vfs_mounted() returns 1 after vfs_mount()");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 2. vfs_open
 * ═══════════════════════════════════════════════════════════════════════════ */

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

/* ═══════════════════════════════════════════════════════════════════════════
 * 3. vfs_read
 * ═══════════════════════════════════════════════════════════════════════════ */

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

/* ═══════════════════════════════════════════════════════════════════════════
 * 4. vfs_close
 * ═══════════════════════════════════════════════════════════════════════════ */

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

/* ═══════════════════════════════════════════════════════════════════════════
 * 5. vfs_readdir
 * ═══════════════════════════════════════════════════════════════════════════ */

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

/* ═══════════════════════════════════════════════════════════════════════════
 * 6. fd table limits
 * ═══════════════════════════════════════════════════════════════════════════ */

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

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════ */

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

    /* fd table limits */
    RUN_SUITE(test_fd_table_full);

    TEST_SUMMARY();
}
