/*
 * Quilon OS — initrd Unit Tests (section 8.3)
 *
 * Tests the RAM-based initial filesystem driver (kernel/kernel/initrd.c).
 * All tests run on the host with native gcc — no QEMU, no cross-compiler.
 *
 * Build & run:  cd tests && make
 */

#include "framework.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/initrd.h>
#include <kernel/vfs.h>

/* ── Image builder helpers ──────────────────────────────────────────────────
 *
 * build_image() constructs a minimal initrd image in `buf` from arrays of
 * names and data strings.  Returns the image size in bytes.
 * ─────────────────────────────────────────────────────────────────────────── */

static uint32_t build_image(uint8_t *buf, const char **names,
                             const char **datas, int n)
{
    uint8_t *p = buf;

    uint32_t count = (uint32_t)n;
    memcpy(p, &count, 4);
    p += 4;

    for (int i = 0; i < n; i++) {
        /* Write name (16 bytes, NUL-padded). */
        char nb[16];
        memset(nb, 0, 16);
        int k;
        for (k = 0; names[i][k] && k < 15; k++) nb[k] = names[i][k];
        memcpy(p, nb, 16);
        p += 16;

        /* Write data size then data. */
        uint32_t sz = (uint32_t)strlen(datas[i]);
        memcpy(p, &sz, 4);
        p += 4;

        memcpy(p, datas[i], sz);
        p += sz;
    }

    return (uint32_t)(p - buf);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 1.  initrd_mount — basic mounting
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_mount_null_ctx(void)
{
    uint8_t img[32] = {0};
    int r = initrd_mount(NULL, img, sizeof(img));
    ASSERT_EQ(r, -1, "initrd_mount with NULL ctx returns -1");
}

static void test_mount_null_addr(void)
{
    initrd_ctx_t ctx;
    int r = initrd_mount(&ctx, NULL, 64);
    ASSERT_EQ(r, -1, "initrd_mount with NULL addr returns -1");
}

static void test_mount_too_small(void)
{
    initrd_ctx_t ctx;
    uint8_t img[2] = {0};
    int r = initrd_mount(&ctx, img, 2);   /* smaller than a uint32_t */
    ASSERT_EQ(r, -1, "initrd_mount with size < 4 returns -1");
}

static void test_mount_zero_files(void)
{
    initrd_ctx_t ctx;
    uint8_t img[4] = {0, 0, 0, 0};   /* file_count = 0 */
    int r = initrd_mount(&ctx, img, 4);
    ASSERT_EQ(r, 0,  "initrd_mount with 0 files succeeds");
    ASSERT_EQ((int)ctx.file_count, 0, "file_count is 0");
}

static void test_mount_one_file(void)
{
    static uint8_t img[64];
    const char *names[] = { "HELLO.TXT" };
    const char *datas[] = { "Hello!" };
    uint32_t sz = build_image(img, names, datas, 1);

    initrd_ctx_t ctx;
    int r = initrd_mount(&ctx, img, sz);
    ASSERT_EQ(r, 0,  "initrd_mount with 1 file returns 0");
    ASSERT_EQ((int)ctx.file_count, 1, "file_count is 1");
    ASSERT_STR_EQ(ctx.files[0].name, "HELLO.TXT", "file name matches");
    ASSERT_EQ((int)ctx.files[0].size, 6, "file size matches strlen('Hello!')");
}

static void test_mount_three_files(void)
{
    static uint8_t img[256];
    const char *names[] = { "A.TXT", "B.TXT", "C.TXT" };
    const char *datas[] = { "alpha",  "beta",  "gamma" };
    uint32_t sz = build_image(img, names, datas, 3);

    initrd_ctx_t ctx;
    int r = initrd_mount(&ctx, img, sz);
    ASSERT_EQ(r, 0, "initrd_mount with 3 files returns 0");
    ASSERT_EQ((int)ctx.file_count, 3, "file_count is 3");
    ASSERT_STR_EQ(ctx.files[0].name, "A.TXT", "files[0].name");
    ASSERT_STR_EQ(ctx.files[1].name, "B.TXT", "files[1].name");
    ASSERT_STR_EQ(ctx.files[2].name, "C.TXT", "files[2].name");
}

static void test_mount_truncated_name(void)
{
    /* Image claims 1 file but has fewer than 20 bytes for the record. */
    static uint8_t img[10];
    memset(img, 0, sizeof(img));
    uint32_t count = 1;
    memcpy(img, &count, 4);
    /* Only 6 bytes left — not enough for name(16)+size(4). */

    initrd_ctx_t ctx;
    int r = initrd_mount(&ctx, img, 10);
    ASSERT_EQ(r, -1, "initrd_mount with truncated record returns -1");
}

static void test_mount_data_past_end(void)
{
    /* File header claims a 100-byte file but image only has 30 bytes total. */
    static uint8_t img[30];
    memset(img, 0, sizeof(img));
    uint32_t count = 1;
    memcpy(img, &count, 4);
    /* Name: 16 bytes of zeros (empty string) */
    uint32_t fsize = 100;
    memcpy(img + 4 + 16, &fsize, 4);   /* claim 100 bytes of data */

    initrd_ctx_t ctx;
    int r = initrd_mount(&ctx, img, 30);
    ASSERT_EQ(r, -1, "initrd_mount rejects file data extending past image end");
}

static void test_mount_too_many_files(void)
{
    /* file_count > INITRD_MAX_FILES (32) should be rejected immediately. */
    static uint8_t img[8];
    uint32_t count = INITRD_MAX_FILES + 1;
    memcpy(img, &count, 4);

    initrd_ctx_t ctx;
    int r = initrd_mount(&ctx, img, 8);
    ASSERT_EQ(r, -1, "initrd_mount rejects file_count > INITRD_MAX_FILES");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 2.  initrd_vfs_ops.open
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_open_existing_file(void)
{
    static uint8_t img[128];
    const char *names[] = { "README.TXT", "NOTES.TXT" };
    const char *datas[] = { "content",    "more"      };
    uint32_t sz = build_image(img, names, datas, 2);

    initrd_ctx_t ctx;
    initrd_mount(&ctx, img, sz);

    vfs_node_t nd = {0};
    int r = initrd_vfs_ops.open(&ctx, "README.TXT", &nd);
    ASSERT_EQ(r, 0, "open existing file returns 0");
    ASSERT_EQ((int)nd.inode,  0, "inode is 0 (first file)");
    ASSERT_EQ((int)nd.size,   7, "size matches 'content' length");
    ASSERT_EQ((int)nd.type,   VFS_TYPE_FILE, "type is VFS_TYPE_FILE");
    ASSERT_EQ((int)nd.in_use, 1, "in_use is 1");
    ASSERT_EQ((int)nd.offset, 0, "offset starts at 0");
}

static void test_open_second_file(void)
{
    static uint8_t img[128];
    const char *names[] = { "README.TXT", "NOTES.TXT" };
    const char *datas[] = { "content",    "more"      };
    uint32_t sz = build_image(img, names, datas, 2);

    initrd_ctx_t ctx;
    initrd_mount(&ctx, img, sz);

    vfs_node_t nd = {0};
    int r = initrd_vfs_ops.open(&ctx, "NOTES.TXT", &nd);
    ASSERT_EQ(r, 0,       "open second file returns 0");
    ASSERT_EQ((int)nd.inode, 1, "inode is 1 (second file)");
}

static void test_open_with_leading_slash(void)
{
    static uint8_t img[64];
    const char *names[] = { "FILE.TXT" };
    const char *datas[] = { "data"     };
    uint32_t sz = build_image(img, names, datas, 1);

    initrd_ctx_t ctx;
    initrd_mount(&ctx, img, sz);

    vfs_node_t nd = {0};
    int r = initrd_vfs_ops.open(&ctx, "/FILE.TXT", &nd);
    ASSERT_EQ(r, 0, "open with leading '/' strips it and finds file");
}

static void test_open_missing_file(void)
{
    static uint8_t img[64];
    const char *names[] = { "FILE.TXT" };
    const char *datas[] = { "data"     };
    uint32_t sz = build_image(img, names, datas, 1);

    initrd_ctx_t ctx;
    initrd_mount(&ctx, img, sz);

    vfs_node_t nd = {0};
    int r = initrd_vfs_ops.open(&ctx, "NOFILE.TXT", &nd);
    ASSERT_EQ(r, -1, "open missing file returns -1");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 3.  initrd_vfs_ops.read
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_read_full_file(void)
{
    static uint8_t img[64];
    const char *names[] = { "MSG.TXT" };
    const char *datas[] = { "Hello, initrd!" };
    uint32_t sz = build_image(img, names, datas, 1);

    initrd_ctx_t ctx;
    initrd_mount(&ctx, img, sz);

    vfs_node_t nd = {0};
    initrd_vfs_ops.open(&ctx, "MSG.TXT", &nd);

    char buf[32];
    int n = initrd_vfs_ops.read(&ctx, &nd, 0, sizeof(buf) - 1, (uint8_t *)buf);
    buf[n] = '\0';

    ASSERT_EQ(n, 14, "read returns full file length");
    ASSERT_STR_EQ(buf, "Hello, initrd!", "read content matches");
}

static void test_read_partial(void)
{
    static uint8_t img[64];
    const char *names[] = { "MSG.TXT" };
    const char *datas[] = { "Hello, initrd!" };
    uint32_t sz = build_image(img, names, datas, 1);

    initrd_ctx_t ctx;
    initrd_mount(&ctx, img, sz);

    vfs_node_t nd = {0};
    initrd_vfs_ops.open(&ctx, "MSG.TXT", &nd);

    char buf[8];
    int n = initrd_vfs_ops.read(&ctx, &nd, 0, 5, (uint8_t *)buf);
    buf[n] = '\0';

    ASSERT_EQ(n, 5, "partial read returns requested byte count");
    ASSERT_STR_EQ(buf, "Hello", "partial read returns correct prefix");
}

static void test_read_with_offset(void)
{
    static uint8_t img[64];
    const char *names[] = { "MSG.TXT" };
    const char *datas[] = { "Hello, initrd!" };
    uint32_t sz = build_image(img, names, datas, 1);

    initrd_ctx_t ctx;
    initrd_mount(&ctx, img, sz);

    vfs_node_t nd = {0};
    initrd_vfs_ops.open(&ctx, "MSG.TXT", &nd);

    char buf[16];
    int n = initrd_vfs_ops.read(&ctx, &nd, 7, sizeof(buf) - 1, (uint8_t *)buf);
    buf[n] = '\0';

    ASSERT_EQ(n, 7, "read from offset returns remaining bytes");
    ASSERT_STR_EQ(buf, "initrd!", "read from offset returns correct suffix");
}

static void test_read_at_eof(void)
{
    static uint8_t img[64];
    const char *names[] = { "MSG.TXT" };
    const char *datas[] = { "Hi" };
    uint32_t sz = build_image(img, names, datas, 1);

    initrd_ctx_t ctx;
    initrd_mount(&ctx, img, sz);

    vfs_node_t nd = {0};
    initrd_vfs_ops.open(&ctx, "MSG.TXT", &nd);

    char buf[8];
    /* offset == file size → EOF */
    int n = initrd_vfs_ops.read(&ctx, &nd, 2, sizeof(buf), (uint8_t *)buf);
    ASSERT_EQ(n, 0, "read at exact EOF returns 0");
}

static void test_read_invalid_inode(void)
{
    static uint8_t img[64];
    const char *names[] = { "A.TXT" };
    const char *datas[] = { "data" };
    uint32_t sz = build_image(img, names, datas, 1);

    initrd_ctx_t ctx;
    initrd_mount(&ctx, img, sz);

    vfs_node_t nd = {0};
    nd.inode = 99;   /* out of range */
    nd.in_use = 1;

    char buf[8];
    int n = initrd_vfs_ops.read(&ctx, &nd, 0, sizeof(buf), (uint8_t *)buf);
    ASSERT_EQ(n, -1, "read with out-of-range inode returns -1");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 4.  initrd_vfs_ops.readdir
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_readdir_first(void)
{
    static uint8_t img[128];
    const char *names[] = { "ALPHA.TXT", "BETA.TXT" };
    const char *datas[] = { "a",          "b"       };
    uint32_t sz = build_image(img, names, datas, 2);

    initrd_ctx_t ctx;
    initrd_mount(&ctx, img, sz);

    vfs_dirent_t ent;
    int r = initrd_vfs_ops.readdir(&ctx, "/", 0, &ent);
    ASSERT_EQ(r, 0, "readdir[0] returns 0");
    ASSERT_STR_EQ(ent.name, "ALPHA.TXT", "readdir[0] name is ALPHA.TXT");
    ASSERT_EQ((int)ent.size, 1, "readdir[0] size is 1");
    ASSERT_EQ((int)ent.type, VFS_TYPE_FILE, "readdir[0] type is FILE");
}

static void test_readdir_second(void)
{
    static uint8_t img[128];
    const char *names[] = { "ALPHA.TXT", "BETA.TXT" };
    const char *datas[] = { "a",          "b"       };
    uint32_t sz = build_image(img, names, datas, 2);

    initrd_ctx_t ctx;
    initrd_mount(&ctx, img, sz);

    vfs_dirent_t ent;
    int r = initrd_vfs_ops.readdir(&ctx, "/", 1, &ent);
    ASSERT_EQ(r, 0, "readdir[1] returns 0");
    ASSERT_STR_EQ(ent.name, "BETA.TXT", "readdir[1] name is BETA.TXT");
}

static void test_readdir_past_end(void)
{
    static uint8_t img[64];
    const char *names[] = { "A.TXT" };
    const char *datas[] = { "x"     };
    uint32_t sz = build_image(img, names, datas, 1);

    initrd_ctx_t ctx;
    initrd_mount(&ctx, img, sz);

    vfs_dirent_t ent;
    int r = initrd_vfs_ops.readdir(&ctx, "/", 1, &ent);
    ASSERT_EQ(r, -1, "readdir past last entry returns -1");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 5.  Read-only: write / create / remove must be NULL
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_ops_are_read_only(void)
{
    ASSERT_NULL((void *)initrd_vfs_ops.write,  "write op is NULL (read-only)");
    ASSERT_NULL((void *)initrd_vfs_ops.create, "create op is NULL (read-only)");
    ASSERT_NULL((void *)initrd_vfs_ops.remove, "remove op is NULL (read-only)");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 6.  initrd_build_demo
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_build_demo_too_small(void)
{
    uint8_t buf[32];
    uint32_t sz = initrd_build_demo(buf, sizeof(buf));
    ASSERT_EQ((int)sz, 0, "initrd_build_demo with bufsize < 256 returns 0");
}

static void test_build_demo_valid(void)
{
    static uint8_t buf[256];
    uint32_t sz = initrd_build_demo(buf, sizeof(buf));
    ASSERT(sz > 0, "initrd_build_demo returns non-zero size");

    initrd_ctx_t ctx;
    int r = initrd_mount(&ctx, buf, sz);
    ASSERT_EQ(r, 0,       "demo image mounts without error");
    ASSERT_EQ((int)ctx.file_count, 3, "demo image has 3 files");
    ASSERT_STR_EQ(ctx.files[0].name, "MOTD.TXT",    "files[0] is MOTD.TXT");
    ASSERT_STR_EQ(ctx.files[1].name, "VERSION.TXT",  "files[1] is VERSION.TXT");
    ASSERT_STR_EQ(ctx.files[2].name, "INIT.SH",      "files[2] is INIT.SH");
}

static void test_build_demo_motd_content(void)
{
    static uint8_t buf[256];
    uint32_t sz = initrd_build_demo(buf, sizeof(buf));

    initrd_ctx_t ctx;
    initrd_mount(&ctx, buf, sz);

    vfs_node_t nd = {0};
    int r = initrd_vfs_ops.open(&ctx, "MOTD.TXT", &nd);
    ASSERT_EQ(r, 0, "open MOTD.TXT in demo image succeeds");

    char content[64];
    int n = initrd_vfs_ops.read(&ctx, &nd, 0, sizeof(content) - 1,
                                 (uint8_t *)content);
    ASSERT(n > 0, "read MOTD.TXT returns bytes");
    content[n] = '\0';
    ASSERT_STR_EQ(content, "Welcome to Quilon OS!", "MOTD.TXT content is correct");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 7.  VFS integration: mount via vfs_mount() and use vfs_* API
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_vfs_mount_and_open(void)
{
    static uint8_t img[128];
    const char *names[] = { "HELLO.TXT" };
    const char *datas[] = { "VFS works!" };
    uint32_t sz = build_image(img, names, datas, 1);

    static initrd_ctx_t ctx;
    initrd_mount(&ctx, img, sz);
    vfs_mount(&initrd_vfs_ops, &ctx);

    ASSERT_EQ(vfs_mounted(), 1, "vfs_mounted() returns 1 after initrd mount");

    int fd = vfs_open("HELLO.TXT");
    ASSERT(fd >= VFS_FD_BASE, "vfs_open returns valid fd for initrd file");
    vfs_close(fd);
}

static void test_vfs_read_through_initrd(void)
{
    static uint8_t img[128];
    const char *names[] = { "HELLO.TXT" };
    const char *datas[] = { "VFS works!" };
    uint32_t sz = build_image(img, names, datas, 1);

    static initrd_ctx_t ctx;
    initrd_mount(&ctx, img, sz);
    vfs_mount(&initrd_vfs_ops, &ctx);

    int fd = vfs_open("HELLO.TXT");
    char buf[32];
    int n = vfs_read(fd, buf, sizeof(buf) - 1);
    buf[n] = '\0';

    ASSERT_EQ(n, 10, "vfs_read returns correct byte count");
    ASSERT_STR_EQ(buf, "VFS works!", "vfs_read returns correct content");
    vfs_close(fd);
}

static void test_vfs_readdir_through_initrd(void)
{
    static uint8_t img[128];
    const char *names[] = { "ONE.TXT", "TWO.TXT" };
    const char *datas[] = { "1",       "2"       };
    uint32_t sz = build_image(img, names, datas, 2);

    static initrd_ctx_t ctx;
    initrd_mount(&ctx, img, sz);
    vfs_mount(&initrd_vfs_ops, &ctx);

    vfs_dirent_t ent;
    ASSERT_EQ(vfs_readdir(0, &ent), 0, "vfs_readdir[0] returns 0");
    ASSERT_STR_EQ(ent.name, "ONE.TXT", "vfs_readdir[0] name is ONE.TXT");
    ASSERT_EQ(vfs_readdir(1, &ent), 0, "vfs_readdir[1] returns 0");
    ASSERT_STR_EQ(ent.name, "TWO.TXT", "vfs_readdir[1] name is TWO.TXT");
    ASSERT_EQ(vfs_readdir(2, &ent), -1, "vfs_readdir past end returns -1");
}

static void test_vfs_write_returns_minus1(void)
{
    static uint8_t img[64];
    const char *names[] = { "RO.TXT" };
    const char *datas[] = { "data"   };
    uint32_t sz = build_image(img, names, datas, 1);

    static initrd_ctx_t ctx;
    initrd_mount(&ctx, img, sz);
    vfs_mount(&initrd_vfs_ops, &ctx);

    int fd = vfs_open("RO.TXT");
    ASSERT(fd >= 0, "open read-only file succeeds");
    int n = vfs_write(fd, "x", 1);
    ASSERT_EQ(n, -1, "vfs_write on initrd returns -1 (read-only)");
    vfs_close(fd);
}

static void test_vfs_create_returns_minus1(void)
{
    static uint8_t img[4] = {0, 0, 0, 0};
    static initrd_ctx_t ctx;
    initrd_mount(&ctx, img, 4);
    vfs_mount(&initrd_vfs_ops, &ctx);

    int r = vfs_create("NEW.TXT");
    ASSERT_EQ(r, -1, "vfs_create on initrd returns -1 (read-only)");
}

static void test_vfs_remove_returns_minus1(void)
{
    static uint8_t img[64];
    const char *names[] = { "DEL.TXT" };
    const char *datas[] = { "data"    };
    uint32_t sz = build_image(img, names, datas, 1);

    static initrd_ctx_t ctx;
    initrd_mount(&ctx, img, sz);
    vfs_mount(&initrd_vfs_ops, &ctx);

    int r = vfs_remove("DEL.TXT");
    ASSERT_EQ(r, -1, "vfs_remove on initrd returns -1 (read-only)");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    /* initrd_mount */
    RUN_SUITE(test_mount_null_ctx);
    RUN_SUITE(test_mount_null_addr);
    RUN_SUITE(test_mount_too_small);
    RUN_SUITE(test_mount_zero_files);
    RUN_SUITE(test_mount_one_file);
    RUN_SUITE(test_mount_three_files);
    RUN_SUITE(test_mount_truncated_name);
    RUN_SUITE(test_mount_data_past_end);
    RUN_SUITE(test_mount_too_many_files);

    /* open */
    RUN_SUITE(test_open_existing_file);
    RUN_SUITE(test_open_second_file);
    RUN_SUITE(test_open_with_leading_slash);
    RUN_SUITE(test_open_missing_file);

    /* read */
    RUN_SUITE(test_read_full_file);
    RUN_SUITE(test_read_partial);
    RUN_SUITE(test_read_with_offset);
    RUN_SUITE(test_read_at_eof);
    RUN_SUITE(test_read_invalid_inode);

    /* readdir */
    RUN_SUITE(test_readdir_first);
    RUN_SUITE(test_readdir_second);
    RUN_SUITE(test_readdir_past_end);

    /* read-only ops */
    RUN_SUITE(test_ops_are_read_only);

    /* initrd_build_demo */
    RUN_SUITE(test_build_demo_too_small);
    RUN_SUITE(test_build_demo_valid);
    RUN_SUITE(test_build_demo_motd_content);

    /* VFS integration */
    RUN_SUITE(test_vfs_mount_and_open);
    RUN_SUITE(test_vfs_read_through_initrd);
    RUN_SUITE(test_vfs_readdir_through_initrd);
    RUN_SUITE(test_vfs_write_returns_minus1);
    RUN_SUITE(test_vfs_create_returns_minus1);
    RUN_SUITE(test_vfs_remove_returns_minus1);

    TEST_SUMMARY();
}
