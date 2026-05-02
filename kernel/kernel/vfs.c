/*
 * Quilon OS — Virtual File System (VFS)
 *
 * Design
 * ──────
 * The VFS is a thin dispatch layer between callers (shell, syscall handler)
 * and a concrete filesystem driver (FAT16, RAM disk, …).  It manages:
 *
 *   • A single "mounted" filesystem described by a vfs_ops_t vtable and an
 *     opaque context pointer.
 *   • A fixed-size file descriptor table of VFS_MAX_FDS slots.
 *     User-visible fds start at VFS_FD_BASE (3) so that 0, 1, 2 remain
 *     reserved for stdin / stdout / stderr.
 *
 * Adding a second mount point, per-directory traversal, or symlinks would
 * require a more elaborate design (a tree of mount points, path resolution
 * loop, …).  For a hobby OS with a flat FAT16 root directory, this single-
 * mount model is more than sufficient.
 */

#include <kernel/vfs.h>

/* ── Mounted filesystem ─────────────────────────────────────────────────── */

static const vfs_ops_t *mounted_ops = (const vfs_ops_t *)0;
static void *mounted_ctx = (void *)0;

/* ── File descriptor table ──────────────────────────────────────────────── */

static vfs_node_t fd_table[VFS_MAX_FDS];

/*
 * fd_to_idx — translate a user-visible fd to a fd_table index.
 *
 * Returns -1 if the fd is out of range or the slot is not open.
 */
static int fd_to_idx(int fd)
{
    int idx = fd - VFS_FD_BASE;
    if (idx < 0 || idx >= VFS_MAX_FDS) return -1;
    if (!fd_table[idx].in_use)         return -1;
    return idx;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void vfs_mount(const vfs_ops_t *ops, void *ctx)
{
    mounted_ops = ops;
    mounted_ctx = ctx;
    /* Close all open fds when a new filesystem is mounted. */
    for (int i = 0; i < VFS_MAX_FDS; i++)
        fd_table[i].in_use = 0;
}

int vfs_mounted(void)
{
    return mounted_ops != (const vfs_ops_t *)0;
}

int vfs_open(const char *path)
{
    if (!vfs_mounted()) return -1;

    /* Find a free fd slot */
    int idx = -1;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fd_table[i].in_use) { idx = i; break; }
    }
    if (idx < 0) return -1;   /* fd table full */

    vfs_node_t node;
    if (mounted_ops->open(mounted_ctx, path, &node) < 0)
        return -1;

    node.in_use = 1;
    node.offset = 0;
    fd_table[idx] = node;
    return idx + VFS_FD_BASE;
}

int vfs_read(int fd, void *buf, uint32_t len)
{
    int idx = fd_to_idx(fd);
    if (idx < 0 || !vfs_mounted()) return -1;

    vfs_node_t *node = &fd_table[idx];

    /* Clamp len to bytes remaining in the file */
    if (node->offset >= node->size) return 0;   /* EOF */
    if (len > node->size - node->offset)
        len = node->size - node->offset;
    if (len == 0) return 0;

    int n = mounted_ops->read(mounted_ctx, node, node->offset,
                              len, (uint8_t *)buf);
    if (n > 0)
        node->offset += (uint32_t)n;
    return n;
}

int vfs_close(int fd)
{
    int idx = fd_to_idx(fd);
    if (idx < 0) return -1;

    if (vfs_mounted())
        mounted_ops->close(mounted_ctx, &fd_table[idx]);
    fd_table[idx].in_use = 0;
    return 0;
}

int vfs_write(int fd, const void *buf, uint32_t len)
{
    int idx = fd_to_idx(fd);
    if (idx < 0 || !vfs_mounted()) return -1;
    if (!mounted_ops->write) return -1;   /* driver has no write support */
    if (!buf || len == 0) return 0;

    vfs_node_t *node = &fd_table[idx];
    int n = mounted_ops->write(mounted_ctx, node, node->offset,
                               len, (const uint8_t *)buf);
    if (n > 0)
        node->offset += (uint32_t)n;
    return n;
}

int vfs_create(const char *path)
{
    if (!vfs_mounted() || !path) return -1;
    if (!mounted_ops->create) return -1;
    return mounted_ops->create(mounted_ctx, path);
}

int vfs_remove(const char *path)
{
    if (!vfs_mounted() || !path) return -1;
    if (!mounted_ops->remove) return -1;
    return mounted_ops->remove(mounted_ctx, path);
}

int vfs_readdir(uint32_t index, vfs_dirent_t *out)
{
    if (!vfs_mounted()) return -1;
    return mounted_ops->readdir(mounted_ctx, index, out);
}
