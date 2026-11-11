/*
 * Quilon OS -- Virtual File System (VFS)
 * Section 12.1: File Metadata & Directory Operations
 *
 * Design
 * ------
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
#include <kernel/spinlock.h>
#ifdef __is_kernel
#include <kernel/pipe.h>
#include <kernel/net.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#endif

/* -- VFS lock (REVIEW.md F) -------------------------------------------------
 *
 * One lock serialises the fd table, the working directory and every call
 * into the mounted driver.  Syscalls are preemptible (int $0x80 is a trap
 * gate and IRQ0 switches tasks even in ring 0), and before this lock:
 *   - two concurrent vfs_open()s could claim the same fd slot -- the slot is
 *     only marked in_use after the driver's directory lookup (several disk
 *     reads), so both processes ended up sharing one fd;
 *   - FAT16 / ext2 keep static scratch buffers (sector_buf, meta_buf, ...)
 *     that a preempted read could find overwritten by another process.
 * ktest's concurrent-read check saw wrong data in ~1 of 200 reads.
 *
 * It is NOT held across pipe or socket I/O: those block (waitq / network
 * polling), and holding the lock there would stall every file operation.
 *
 * Waiting: only one CPU schedules, so spinning while the holder is preempted
 * would just burn the waiter's slice.  A waiter marks itself READY and
 * yields, which runs the holder.  Not IRQ-safe by design: the VFS is never
 * entered from interrupt context, and a ring-0 page fault taken inside a
 * driver (copying into a user buffer) does not touch the VFS.
 */
static spinlock_t vfs_lk = SPINLOCK_INIT;
static int        vfs_lock_contended;   /* host tests: nested/unbalanced use */

static void vfs_lock(void)
{
    while (!spinlock_trylock(&vfs_lk)) {
#ifdef __is_kernel
        if (current_process) {
            current_process->state = PROC_READY;
            scheduler_yield();
            current_process->state = PROC_RUNNING;
        } else {
            asm volatile("pause");
        }
#else
        /* Single-threaded host tests can only get here by taking the lock
         * twice (or never releasing it) -- record it instead of hanging. */
        vfs_lock_contended++;
        spinlock_release(&vfs_lk);
#endif
    }
}

static void vfs_unlock(void) { spinlock_release(&vfs_lk); }

int vfs_lock_is_held(void)      { return spinlock_is_locked(&vfs_lk); }
int vfs_lock_contentions(void)  { return vfs_lock_contended; }

/* Find a free fd slot; caller holds the VFS lock.  -1 if the table is full. */
static int fd_alloc_locked(void);

/* -- Mounted filesystem --------------------------------------------------- */

static const vfs_ops_t *mounted_ops = (const vfs_ops_t *)0;
static void *mounted_ctx = (void *)0;

/* -- Working directory (section 12.1) ---------------------------------------- */

static char vfs_cwd[VFS_PATH_MAX] = "/";

/* -- Path resolution ------------------------------------------------------
 *
 * Every path handed to a driver is first turned into a normalised absolute
 * path: relative paths are joined onto the working directory, and "." and
 * ".." are folded away.  Before ext2 (section 16.2) this did not matter --
 * FAT16 has only a root directory, so every path was effectively absolute --
 * but with real subdirectories `cd sub` followed by `cat file.txt` has to
 * find /sub/file.txt.
 *
 * The invariant that makes this cheap: vfs_cwd is always already normalised
 * and absolute, so a relative path only has to be appended to it.
 */

/* Drop the last component of `out`, leaving at least "/". */
static void path_pop(char *out, uint32_t *len)
{
    while (*len > 1 && out[*len - 1] != '/') (*len)--;
    if (*len > 1) (*len)--;   /* also drop the separating '/' */
}

/* Append one component.  Returns -1 if it would not fit. */
static int path_push(char *out, uint32_t *len, const char *comp,
                     uint32_t clen, uint32_t outlen)
{
    uint32_t need = clen + (*len > 1 ? 1u : 0u);
    if (*len + need + 1 > outlen) return -1;

    if (*len > 1) out[(*len)++] = '/';
    for (uint32_t i = 0; i < clen; i++) out[(*len)++] = comp[i];
    return 0;
}

/*
 * vfs_normalize -- resolve `path` against the CWD into `out`.
 *
 * Writes a normalised absolute path ("/", "/a", "/a/b" -- never a trailing
 * slash, never "." or ".." components).  Returns 0 on success, -1 if the
 * result would not fit in `outlen`.
 */
static int vfs_normalize(const char *path, char *out, uint32_t outlen)
{
    if (!path || outlen < 2) return -1;

    uint32_t len = 0;
    out[len++] = '/';

    /* A relative path starts from the (already normalised) CWD. */
    if (path[0] != '/') {
        for (uint32_t i = 1; vfs_cwd[i]; i++) {
            if (len + 1 >= outlen) return -1;
            out[len++] = vfs_cwd[i];
        }
    }

    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        const char *start = p;
        while (*p && *p != '/') p++;
        uint32_t clen = (uint32_t)(p - start);
        if (clen == 0) continue;

        if (clen == 1 && start[0] == '.')
            continue;                                  /* "." -- no-op */
        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            path_pop(out, &len);                       /* ".." -- go up */
            continue;
        }
        if (path_push(out, &len, start, clen, outlen) < 0) return -1;
    }

    out[len] = '\0';
    return 0;
}

/* -- File descriptor table ------------------------------------------------ */

static vfs_node_t fd_table[VFS_MAX_FDS];

/*
 * fd_to_idx -- translate a user-visible fd to a fd_table index.
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

/* -- Public API ----------------------------------------------------------- */

static int fd_alloc_locked(void)
{
    for (int i = 0; i < VFS_MAX_FDS; i++)
        if (!fd_table[i].in_use) return i;
    return -1;
}

void vfs_mount(const vfs_ops_t *ops, void *ctx)
{
    vfs_lock();
    mounted_ops = ops;
    mounted_ctx = ctx;
    /* Close all open fds when a new filesystem is mounted. */
    for (int i = 0; i < VFS_MAX_FDS; i++)
        fd_table[i].in_use = 0;
    vfs_unlock();
}

int vfs_mounted(void)
{
    return mounted_ops != (const vfs_ops_t *)0;
}

int vfs_open(const char *path)
{
    if (!vfs_mounted()) return -1;

    /* Slot choice, driver lookup and install happen under one lock: the slot
     * used to be claimed only after the (preemptible) lookup, so two opens
     * could both pick it. */
    vfs_lock();
    int idx = fd_alloc_locked();
    char abs[VFS_PATH_MAX];
    vfs_node_t node = {0};   /* zero all fields including is_pipe */
    if (idx < 0 || vfs_normalize(path, abs, sizeof abs) < 0 ||
        mounted_ops->open(mounted_ctx, abs, &node) < 0) {
        vfs_unlock();
        return -1;
    }
    node.in_use = 1;
    node.offset = 0;
    fd_table[idx] = node;
    vfs_unlock();
    return idx + VFS_FD_BASE;
}

/* Bytes immediately readable from fd without blocking.
 * Pipe fd: queued bytes (0 if empty).  Regular file: bytes to EOF.
 * Returns -1 for an invalid fd. */
int vfs_readable(int fd)
{
    int idx = fd_to_idx(fd);
    if (idx < 0) return -1;
    vfs_node_t *node = &fd_table[idx];
#ifdef __is_kernel
    if (node->is_pipe)
        return (int)pipe_bytes_available((int)node->pipe_idx);
    if (node->is_socket)
        return (node->sock_type == SOCK_STREAM) ? net_tcp_readable() : 0;
#endif
    if (!vfs_mounted()) return 0;
    vfs_lock();
    int avail = (node->offset >= node->size) ? 0
              : (int)(node->size - node->offset);
    vfs_unlock();
    return avail;
}

int vfs_read(int fd, void *buf, uint32_t len)
{
    int idx = fd_to_idx(fd);
    if (idx < 0) return -1;

    vfs_node_t *node = &fd_table[idx];

#ifdef __is_kernel
    if (node->is_pipe)
        return pipe_read((int)node->pipe_idx, (uint8_t *)buf, len);
    if (node->is_socket) {
        if (len > 0xFFFFu) len = 0xFFFFu;
        if (node->sock_type == SOCK_STREAM)
            return net_tcp_recv(buf, (uint16_t)len);
        return net_udp_recv(node->sock_lport, buf, (uint16_t)len);
    }
#endif

    if (!vfs_mounted()) return -1;

    vfs_lock();
    if (fd_to_idx(fd) != idx) { vfs_unlock(); return -1; }  /* closed meanwhile */

    /* Clamp len to bytes remaining in the file */
    int n = 0;
    if (node->offset < node->size) {
        if (len > node->size - node->offset)
            len = node->size - node->offset;
        if (len > 0)
            n = mounted_ops->read(mounted_ctx, node, node->offset,
                                  len, (uint8_t *)buf);
        if (n > 0)
            node->offset += (uint32_t)n;
    }
    vfs_unlock();
    return n;
}

int vfs_close(int fd)
{
    /* Locked so a slot is never freed while another process is installing or
     * using it.  Pipe/socket close does not block, so it can run locked. */
    vfs_lock();
    int idx = fd_to_idx(fd);
    if (idx < 0) { vfs_unlock(); return -1; }

    vfs_node_t *node = &fd_table[idx];

#ifdef __is_kernel
    if (node->is_pipe) {
        if (node->pipe_write_end)
            pipe_close_write((int)node->pipe_idx);
        else
            pipe_close_read((int)node->pipe_idx);
        node->in_use = 0;
        vfs_unlock();
        return 0;
    }
    if (node->is_socket) {
        if (node->sock_type == SOCK_STREAM)
            net_tcp_close();
        node->in_use = 0;
        vfs_unlock();
        return 0;
    }
#endif

    if (vfs_mounted())
        mounted_ops->close(mounted_ctx, node);
    node->in_use = 0;
    vfs_unlock();
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
    if (node->is_socket) {
        if (!buf || len == 0) return 0;
        if (len > 0xFFFFu) len = 0xFFFFu;
        if (node->sock_type == SOCK_STREAM)
            return (net_tcp_send(buf, (uint16_t)len) == 0) ? (int)len : -1;
        return (net_udp_send(node->sock_rip, node->sock_lport,
                             node->sock_rport, buf, (uint16_t)len) == 0)
               ? (int)len : -1;
    }
#endif

    if (!vfs_mounted()) return -1;
    if (!mounted_ops->write) return -1;   /* driver has no write support */
    if (!buf || len == 0) return 0;

    vfs_lock();
    if (fd_to_idx(fd) != idx) { vfs_unlock(); return -1; }  /* closed meanwhile */
    int n = mounted_ops->write(mounted_ctx, node, node->offset,
                               len, (const uint8_t *)buf);
    if (n > 0)
        node->offset += (uint32_t)n;
    vfs_unlock();
    return n;
}

int vfs_create(const char *path)
{
    if (!vfs_mounted() || !path) return -1;
    if (!mounted_ops->create) return -1;

    char abs[VFS_PATH_MAX];
    vfs_lock();
    int r = (vfs_normalize(path, abs, sizeof abs) < 0) ? -1
          : mounted_ops->create(mounted_ctx, abs);
    vfs_unlock();
    return r;
}

int vfs_remove(const char *path)
{
    if (!vfs_mounted() || !path) return -1;
    if (!mounted_ops->remove) return -1;

    char abs[VFS_PATH_MAX];
    vfs_lock();
    int r = (vfs_normalize(path, abs, sizeof abs) < 0) ? -1
          : mounted_ops->remove(mounted_ctx, abs);
    vfs_unlock();
    return r;
}

int vfs_readdir(uint32_t index, vfs_dirent_t *out)
{
    if (!vfs_mounted() || !out) return -1;
    if (!mounted_ops->readdir) return -1;
    vfs_lock();
    int r = mounted_ops->readdir(mounted_ctx, vfs_cwd, index, out);
    vfs_unlock();
    return r;
}

/* -- Section 12.1 -- file metadata & directory ops ------------------------- */

static int vfs_lseek_locked(int fd, int32_t offset, int whence);

int vfs_lseek(int fd, int32_t offset, int whence)
{
    vfs_lock();
    int r = vfs_lseek_locked(fd, offset, whence);
    vfs_unlock();
    return r;
}

static int vfs_lseek_locked(int fd, int32_t offset, int whence)
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

    char abs[VFS_PATH_MAX];
    vfs_lock();
    int r = (vfs_normalize(path, abs, sizeof abs) < 0) ? -1
          : mounted_ops->stat(mounted_ctx, abs, out);
    vfs_unlock();
    return r;
}

int vfs_mkdir(const char *path)
{
    if (!vfs_mounted() || !path) return -1;
    if (!mounted_ops->mkdir) return -1;

    char abs[VFS_PATH_MAX];
    vfs_lock();
    int r = (vfs_normalize(path, abs, sizeof abs) < 0) ? -1
          : mounted_ops->mkdir(mounted_ctx, abs);
    vfs_unlock();
    return r;
}

int vfs_rename(const char *oldpath, const char *newpath)
{
    if (!vfs_mounted() || !oldpath || !newpath) return -1;
    if (!mounted_ops->rename) return -1;

    char oldabs[VFS_PATH_MAX], newabs[VFS_PATH_MAX];
    vfs_lock();
    int r = (vfs_normalize(oldpath, oldabs, sizeof oldabs) < 0 ||
             vfs_normalize(newpath, newabs, sizeof newabs) < 0) ? -1
          : mounted_ops->rename(mounted_ctx, oldabs, newabs);
    vfs_unlock();
    return r;
}

int vfs_fsinfo(vfs_fsinfo_t *out)
{
    if (!vfs_mounted() || !out) return -1;
    if (!mounted_ops->fsinfo) return -1;
    vfs_lock();
    int r = mounted_ops->fsinfo(mounted_ctx, out);
    vfs_unlock();
    return r;
}

static int vfs_chdir_locked(const char *path);

int vfs_chdir(const char *path)
{
    if (!path) return -1;
    vfs_lock();
    int r = vfs_chdir_locked(path);
    vfs_unlock();
    return r;
}

static int vfs_chdir_locked(const char *path)
{
    /* Resolve first, so `cd sub`, `cd ..` and `cd /a/../b` all end up as a
     * clean absolute path -- vfs_normalize relies on vfs_cwd already being
     * one, and getcwd should report one. */
    char abs[VFS_PATH_MAX];
    if (vfs_normalize(path, abs, sizeof abs) < 0) return -1;

    /* Validate the directory exists if the driver supports stat.
     * "/" always exists and need not be probed. */
    if (abs[1] != '\0' && vfs_mounted() && mounted_ops->stat) {
        vfs_stat_t st;
        if (mounted_ops->stat(mounted_ctx, abs, &st) != 0) return -1;
        if (st.type != VFS_TYPE_DIR) return -1;
    }

    uint32_t i = 0;
    while (i < VFS_PATH_MAX - 1 && abs[i]) {
        vfs_cwd[i] = abs[i];
        i++;
    }
    vfs_cwd[i] = '\0';
    return 0;
}

int vfs_getcwd(char *buf, uint32_t len)
{
    if (!buf || len == 0) return -1;
    vfs_lock();
    uint32_t i = 0;
    while (i < len - 1 && vfs_cwd[i]) { buf[i] = vfs_cwd[i]; i++; }
    buf[i] = '\0';
    vfs_unlock();
    return 0;
}

int vfs_pipe(int fds[2])
{
    if (!fds) return -1;

#ifdef __is_kernel
    int pipe_idx = pipe_alloc();
    if (pipe_idx < 0) return -1;

    /* Find two free fd slots. */
    vfs_lock();
    int rslot = -1, wslot = -1;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fd_table[i].in_use) {
            if (rslot < 0)       rslot = i;
            else if (wslot < 0) { wslot = i; break; }
        }
    }
    if (rslot < 0 || wslot < 0) {
        vfs_unlock();
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

    vfs_unlock();
    fds[0] = rslot + VFS_FD_BASE;   /* read end  */
    fds[1] = wslot + VFS_FD_BASE;   /* write end */
    return 0;
#else
    (void)fds;
    return -1;
#endif
}

vfs_node_t *vfs_node_for_fd(int fd)
{
    int idx = fd_to_idx(fd);
    if (idx < 0) return (vfs_node_t *)0;
    return &fd_table[idx];
}

int vfs_socket(int type)
{
#ifdef __is_kernel
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -1;

    vfs_lock();
    int idx = fd_alloc_locked();
    if (idx < 0) { vfs_unlock(); return -1; }   /* fd table full */

    vfs_node_t node = {0};
    node.in_use    = 1;
    node.is_socket = 1;
    node.sock_type = (uint8_t)type;
    fd_table[idx]  = node;
    vfs_unlock();
    return idx + VFS_FD_BASE;
#else
    (void)type;
    return -1;
#endif
}
