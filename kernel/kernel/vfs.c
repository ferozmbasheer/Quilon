/*
 * Quilon OS — Virtual File System (VFS)
 * Section 12.1: File Metadata & Directory Operations
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
#ifdef __is_kernel
#include <kernel/pipe.h>
#endif

/* ── Mounted filesystem ─────────────────────────────────────────────────── */

static const vfs_ops_t *mounted_ops = (const vfs_ops_t *)0;
static void *mounted_ctx = (void *)0;

/* ── Working directory (section 12.1) ──────────────────────────────────────── */

static char vfs_cwd[VFS_PATH_MAX] = "/";

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

    vfs_node_t node = {0};   /* zero all fields including is_pipe */
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
    if (idx < 0) return -1;

    vfs_node_t *node = &fd_table[idx];

#ifdef __is_kernel
    if (node->is_pipe)
        return pipe_read((int)node->pipe_idx, (uint8_t *)buf, len);
#endif

    if (!vfs_mounted()) return -1;

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

    vfs_node_t *node = &fd_table[idx];

#ifdef __is_kernel
    if (node->is_pipe) {
        if (node->pipe_write_end)
            pipe_close_write((int)node->pipe_idx);
        else
            pipe_close_read((int)node->pipe_idx);
        node->in_use = 0;
        return 0;
    }
#endif

    if (vfs_mounted())
        mounted_ops->close(mounted_ctx, node);
    node->in_use = 0;
    return 0;
}

int vfs_write(int fd, const void *buf, uint32_t len)
{
    int idx = fd_to_idx(fd);
    if (idx < 0) return -1;

    vfs_node_t *node = &fd_table[idx];

#ifdef __is_kernel
    if (node->is_pipe)
        return pipe_write((int)node->pipe_idx, (const uint8_t *)buf, len);
#endif

    if (!vfs_mounted()) return -1;
    if (!mounted_ops->write) return -1;   /* driver has no write support */
    if (!buf || len == 0) return 0;

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
    if (!vfs_mounted() || !out) return -1;
    if (!mounted_ops->readdir) return -1;
    return mounted_ops->readdir(mounted_ctx, vfs_cwd, index, out);
}

/* ── Section 12.1 — file metadata & directory ops ───────────────────────── */

int vfs_lseek(int fd, int32_t offset, int whence)
{
    int idx = fd_to_idx(fd);
    if (idx < 0) return -1;

    vfs_node_t *node = &fd_table[idx];
    if (node->is_pipe) return -1;

    uint32_t new_pos;
    switch (whence) {
    case VFS_SEEK_SET:
        if (offset < 0) return -1;
        new_pos = (uint32_t)offset;
        break;
    case VFS_SEEK_CUR:
        if (offset < 0) {
            uint32_t abs = (uint32_t)(-offset);
            if (abs > node->offset) return -1;
            new_pos = node->offset - abs;
        } else {
            new_pos = node->offset + (uint32_t)offset;
        }
        break;
    case VFS_SEEK_END:
        if (offset < 0) {
            uint32_t abs = (uint32_t)(-offset);
            if (abs > node->size) return -1;
            new_pos = node->size - abs;
        } else {
            new_pos = node->size + (uint32_t)offset;
        }
        break;
    default:
        return -1;
    }

    node->offset = new_pos;
    return (int)new_pos;
}

int vfs_stat(const char *path, vfs_stat_t *out)
{
    if (!vfs_mounted() || !path || !out) return -1;
    if (!mounted_ops->stat) return -1;
    return mounted_ops->stat(mounted_ctx, path, out);
}

int vfs_mkdir(const char *path)
{
    if (!vfs_mounted() || !path) return -1;
    if (!mounted_ops->mkdir) return -1;
    return mounted_ops->mkdir(mounted_ctx, path);
}

int vfs_rename(const char *oldpath, const char *newpath)
{
    if (!vfs_mounted() || !oldpath || !newpath) return -1;
    if (!mounted_ops->rename) return -1;
    return mounted_ops->rename(mounted_ctx, oldpath, newpath);
}

int vfs_chdir(const char *path)
{
    if (!path) return -1;

    if (path[0] == '/' && path[1] == '\0') {
        vfs_cwd[0] = '/'; vfs_cwd[1] = '\0';
        return 0;
    }

    /* Validate the directory exists if the driver supports stat. */
    if (vfs_mounted() && mounted_ops->stat) {
        vfs_stat_t st;
        if (mounted_ops->stat(mounted_ctx, path, &st) != 0) return -1;
        if (st.type != VFS_TYPE_DIR) return -1;
    }

    uint32_t i = 0;
    while (i < VFS_PATH_MAX - 1 && path[i]) {
        vfs_cwd[i] = path[i];
        i++;
    }
    vfs_cwd[i] = '\0';
    return 0;
}

int vfs_getcwd(char *buf, uint32_t len)
{
    if (!buf || len == 0) return -1;
    uint32_t i = 0;
    while (i < len - 1 && vfs_cwd[i]) { buf[i] = vfs_cwd[i]; i++; }
    buf[i] = '\0';
    return 0;
}

int vfs_pipe(int fds[2])
{
    if (!fds) return -1;

#ifdef __is_kernel
    int pipe_idx = pipe_alloc();
    if (pipe_idx < 0) return -1;

    /* Find two free fd slots. */
    int rslot = -1, wslot = -1;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fd_table[i].in_use) {
            if (rslot < 0)       rslot = i;
            else if (wslot < 0) { wslot = i; break; }
        }
    }
    if (rslot < 0 || wslot < 0) {
        pipe_close_read(pipe_idx);
        pipe_close_write(pipe_idx);
        return -1;
    }

    /* Initialise read end. */
    vfs_node_t *r    = &fd_table[rslot];
    r->in_use        = 1;
    r->is_pipe       = 1;
    r->pipe_write_end = 0;
    r->pipe_idx      = (uint8_t)pipe_idx;
    r->inode         = 0; r->size = 0; r->type = 0;
    r->offset        = 0; r->dir_sector = 0; r->dir_entry_idx = 0;

    /* Initialise write end. */
    vfs_node_t *w    = &fd_table[wslot];
    w->in_use        = 1;
    w->is_pipe       = 1;
    w->pipe_write_end = 1;
    w->pipe_idx      = (uint8_t)pipe_idx;
    w->inode         = 0; w->size = 0; w->type = 0;
    w->offset        = 0; w->dir_sector = 0; w->dir_entry_idx = 0;

    fds[0] = rslot + VFS_FD_BASE;   /* read end  */
    fds[1] = wslot + VFS_FD_BASE;   /* write end */
    return 0;
#else
    (void)fds;
    return -1;
#endif
}
