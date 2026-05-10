/*
 * Quilon OS — initrd: RAM-Based Initial Filesystem (section 8.3)
 *
 * A read-only VFS driver backed by a flat in-memory image.  The image can
 * come from a GRUB Multiboot module or be built synthetically in kernel RAM.
 * Because it lives entirely in memory, it is available before any disk
 * driver is initialized — a diskless kernel can still boot to a shell.
 *
 * Image format (all integers are little-endian uint32_t):
 *
 *   [ uint32_t  file_count N                                    ]
 *   [ N × { char     name[16];   (NUL-padded, not NUL-terminated)
 *            uint32_t data_size;
 *            uint8_t  data[data_size]; }                        ]
 *
 * The driver does NOT copy file data; initrd_entry_t.data points directly
 * into the raw image.  The caller must keep the image buffer alive.
 */

#include <stdint.h>
#include <string.h>
#include <kernel/initrd.h>
#include <kernel/vfs.h>

/* ── VFS driver callbacks ──────────────────────────────────────────────────── */

static int initrd_open(void *ctx, const char *path, vfs_node_t *out)
{
    initrd_ctx_t *rd = (initrd_ctx_t *)ctx;
    if (*path == '/') path++;   /* strip optional leading slash */

    for (uint32_t i = 0; i < rd->file_count; i++) {
        if (strcmp(path, rd->files[i].name) == 0) {
            out->inode  = i;
            out->size   = rd->files[i].size;
            out->type   = VFS_TYPE_FILE;
            out->in_use = 1;
            out->offset = 0;
            return 0;
        }
    }
    return -1;
}

static int initrd_read(void *ctx, vfs_node_t *node, uint32_t offset,
                       uint32_t size, uint8_t *buf)
{
    initrd_ctx_t *rd  = (initrd_ctx_t *)ctx;
    uint32_t      idx = node->inode;

    if (idx >= rd->file_count)          return -1;
    if (offset >= rd->files[idx].size)  return 0;   /* EOF */

    uint32_t avail = rd->files[idx].size - offset;
    if (size > avail) size = avail;

    memcpy(buf, rd->files[idx].data + offset, size);
    return (int)size;
}

static int initrd_readdir(void *ctx, const char *path, uint32_t index,
                           vfs_dirent_t *out)
{
    (void)path;   /* initrd is a flat filesystem — path is always "/" */
    initrd_ctx_t *rd = (initrd_ctx_t *)ctx;
    if (index >= rd->file_count) return -1;

    const char *src = rd->files[index].name;
    int i = 0;
    while (*src && i < VFS_NAME_MAX) out->name[i++] = *src++;
    out->name[i] = '\0';

    out->size = rd->files[index].size;
    out->type = VFS_TYPE_FILE;
    return 0;
}

static void initrd_close(void *ctx, vfs_node_t *node)
{
    (void)ctx; (void)node;   /* nothing to release — data lives in the image */
}

const vfs_ops_t initrd_vfs_ops = {
    .open    = initrd_open,
    .read    = initrd_read,
    .write   = NULL,    /* read-only filesystem */
    .readdir = initrd_readdir,
    .close   = initrd_close,
    .create  = NULL,    /* read-only filesystem */
    .remove  = NULL,    /* read-only filesystem */
};

/* ── initrd_mount ──────────────────────────────────────────────────────────── */

int initrd_mount(initrd_ctx_t *ctx, void *addr, uint32_t size)
{
    if (!ctx || !addr || size < sizeof(uint32_t))
        return -1;

    uint8_t *p   = (uint8_t *)addr;
    uint8_t *end = p + size;

    uint32_t count;
    memcpy(&count, p, sizeof(uint32_t));
    p += sizeof(uint32_t);

    if (count > INITRD_MAX_FILES)
        return -1;

    ctx->base       = (uint8_t *)addr;
    ctx->image_size = size;
    ctx->file_count = 0;

    for (uint32_t i = 0; i < count; i++) {
        /* Each record needs at least 16 (name) + 4 (size) bytes. */
        if (p + 20 > end) return -1;

        memcpy(ctx->files[i].name, p, 16);
        ctx->files[i].name[15] = '\0';   /* guarantee NUL termination */
        p += 16;

        uint32_t fsize;
        memcpy(&fsize, p, sizeof(uint32_t));
        p += sizeof(uint32_t);

        if (p + fsize > end) return -1;  /* data extends past image end */

        ctx->files[i].size = fsize;
        ctx->files[i].data = p;
        p += fsize;
        ctx->file_count++;
    }

    return 0;
}

/* ── initrd_build_demo ─────────────────────────────────────────────────────── */

uint32_t initrd_build_demo(uint8_t *buf, uint32_t bufsize)
{
    static const struct { const char *name; const char *data; } entries[] = {
        { "MOTD.TXT",    "Welcome to Quilon OS!"  },
        { "VERSION.TXT", "Quilon OS r0.8.3"       },
        { "INIT.SH",     "exec SHELL.ELF"         },
    };
    const uint32_t NFILES = 3;

    if (bufsize < 256) return 0;

    uint8_t *p = buf;

    memcpy(p, &NFILES, 4);
    p += 4;

    for (uint32_t i = 0; i < NFILES; i++) {
        char nb[16];
        memset(nb, 0, 16);
        for (int k = 0; entries[i].name[k] && k < 15; k++)
            nb[k] = entries[i].name[k];
        memcpy(p, nb, 16);
        p += 16;

        uint32_t sz = (uint32_t)strlen(entries[i].data);
        memcpy(p, &sz, 4);
        p += 4;

        memcpy(p, entries[i].data, sz);
        p += sz;
    }

    return (uint32_t)(p - buf);
}
