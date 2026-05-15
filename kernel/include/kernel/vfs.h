#ifndef _KERNEL_VFS_H
#define _KERNEL_VFS_H

#include <stdint.h>

/* -- Constants ------------------------------------------------------------ */

#define VFS_NAME_MAX   12   /* 8.3 name: 8 chars + '.' + 3 chars + NUL */
#define VFS_PATH_MAX  128   /* maximum path length including NUL        */
#define VFS_MAX_FDS     8   /* maximum simultaneously open files        */
#define VFS_FD_BASE     3   /* first fd returned by vfs_open;
                               0-2 are reserved for stdin/stdout/stderr */

/* vfs_node_t.type values */
#define VFS_TYPE_FILE  0
#define VFS_TYPE_DIR   1

/* vfs_lseek whence constants */
#define VFS_SEEK_SET  0   /* seek from start of file   */
#define VFS_SEEK_CUR  1   /* seek from current position */
#define VFS_SEEK_END  2   /* seek from end of file      */

/*
 * vfs_stat_t -- minimal file metadata returned by vfs_stat().
 *
 * Layout must match the user-space stat_t in user/libc/include/sys/stat.h
 * exactly, because SYS_STAT writes this struct to a user-space pointer.
 */
typedef struct {
    uint32_t size;   /* file size in bytes (0 for directories) */
    uint8_t  type;   /* VFS_TYPE_FILE or VFS_TYPE_DIR          */
} vfs_stat_t;

/* -- Types ---------------------------------------------------------------- */

/*
 * vfs_node_t -- open file state.
 *
 * An entry in the global file descriptor table.  The driver fills this in
 * during open(); the driver reads it back on read(), write(), and close().
 *
 * `inode` is opaque to the VFS layer: the FAT16 driver stores the starting
 * cluster number here.
 * `dir_sector` and `dir_entry_idx` let the FAT16 driver update the directory
 * entry (size, first_cluster) on write without re-scanning.  Other drivers
 * may leave them zero.
 * `offset` is maintained by the VFS layer and advanced on each read/write.
 */
typedef struct {
    uint32_t  inode;          /* driver-internal node identifier (e.g. cluster) */
    uint32_t  size;           /* file size in bytes                              */
    uint8_t   type;           /* VFS_TYPE_FILE or VFS_TYPE_DIR                   */
    uint8_t   in_use;         /* 1 = slot occupied, 0 = free                     */
    uint32_t  offset;         /* current byte position for reads/writes          */
    uint32_t  dir_sector;     /* LBA of the sector holding this file's dir entry */
    uint8_t   dir_entry_idx;  /* index of the dir entry within that sector       */
    /* Pipe support (section 8.2) */
    uint8_t   is_pipe;        /* 1 = pipe fd, 0 = regular file fd                */
    uint8_t   pipe_write_end; /* 1 = write end, 0 = read end (when is_pipe = 1)  */
    uint8_t   pipe_idx;       /* index into pipe_pool[] (when is_pipe = 1)       */
} vfs_node_t;

/*
 * vfs_dirent_t -- a directory entry as returned by vfs_readdir().
 */
typedef struct {
    char     name[VFS_NAME_MAX + 1];  /* NUL-terminated 8.3 name, e.g. "FOO.TXT" */
    uint32_t size;                    /* file size in bytes                       */
    uint8_t  type;                    /* VFS_TYPE_FILE or VFS_TYPE_DIR            */
} vfs_dirent_t;

/*
 * vfs_ops_t -- filesystem driver vtable.
 *
 * Registered once via vfs_mount().  The VFS layer calls these functions and
 * passes the opaque `ctx` pointer supplied to vfs_mount() on every call.
 *
 *   open:    Locate `path` in the filesystem.  Fill `*out` and return 0 on
 *            success; return -1 if not found.
 *   read:    Copy `size` bytes starting at byte `offset` of `node` into
 *            `buf`.  Return bytes actually read, or -1 on error.
 *   write:   Write `size` bytes from `buf` into `node` starting at `offset`.
 *            Extends the file and updates the directory entry if needed.
 *            Return bytes written, or -1 on error.  NULL = write unsupported.
 *   readdir: Return the `index`-th directory entry in `*out` (index starts
 *            at 0).  Return 0 on success, -1 at end-of-directory.
 *   close:   Release driver-side resources for this node.  May be a no-op.
 *   create:  Create a new empty file at `path`.  Return 0 on success, -1 on
 *            error (name invalid, disk full, already exists).  NULL = unsupported.
 *   remove:  Delete the file at `path`: free its cluster chain and mark its
 *            directory entry deleted.  Return 0 on success, -1 on error.
 *            NULL = unsupported.
 */
typedef struct {
    int  (*open)   (void *ctx, const char *path, vfs_node_t *out);
    int  (*read)   (void *ctx, vfs_node_t *node, uint32_t offset,
                    uint32_t size, uint8_t *buf);
    int  (*write)  (void *ctx, vfs_node_t *node, uint32_t offset,
                    uint32_t size, const uint8_t *buf);
    int  (*readdir)(void *ctx, const char *path, uint32_t index, vfs_dirent_t *out);
    void (*close)  (void *ctx, vfs_node_t *node);
    int  (*create) (void *ctx, const char *path);
    int  (*remove) (void *ctx, const char *path);
    /* Section 12.1 -- file metadata & directory ops */
    int  (*stat)   (void *ctx, const char *path, vfs_stat_t *out);
    int  (*mkdir)  (void *ctx, const char *path);
    int  (*rename) (void *ctx, const char *oldpath, const char *newpath);
} vfs_ops_t;

/* -- Public API ----------------------------------------------------------- */

/*
 * vfs_mount -- register a filesystem driver.
 *
 * `ops` and `ctx` must remain valid for the lifetime of the kernel.
 * Only one mounted filesystem is supported (single-mount design).
 * Calling vfs_mount() a second time replaces the previous mount.
 */
void vfs_mount(const vfs_ops_t *ops, void *ctx);

/* Returns 1 if a filesystem is currently mounted, 0 otherwise. */
int  vfs_mounted(void);

/*
 * vfs_open -- open a file by path.
 *
 * Returns a file descriptor (>= VFS_FD_BASE) on success, -1 on failure
 * (no filesystem mounted, file not found, or fd table full).
 */
int  vfs_open(const char *path);

/*
 * vfs_read -- read up to `len` bytes from `fd` into `buf`.
 *
 * Advances the internal offset.  Returns bytes read (0 = EOF), -1 on error.
 */
int  vfs_read(int fd, void *buf, uint32_t len);

/* vfs_close -- release a file descriptor.  Returns 0 or -1. */
int  vfs_close(int fd);

/*
 * vfs_write -- write up to `len` bytes from `buf` into `fd`.
 *
 * Advances the internal offset.  Extends the file if writing past EOF.
 * Returns bytes written, or -1 if the driver does not support writes.
 */
int  vfs_write(int fd, const void *buf, uint32_t len);

/*
 * vfs_create -- create a new empty file at `path`.
 *
 * Returns 0 on success, -1 on failure (no FS, unsupported, name invalid,
 * disk full, or the file already exists).
 */
int  vfs_create(const char *path);

/*
 * vfs_remove -- delete the file at `path`.
 *
 * Returns 0 on success, -1 on failure.
 */
int  vfs_remove(const char *path);

/*
 * vfs_readdir -- read the `index`-th directory entry into `*out`.
 *
 * Returns 0 on success, -1 on end-of-directory or error.
 * index = 0 is the first entry.
 */
int  vfs_readdir(uint32_t index, vfs_dirent_t *out);

/*
 * vfs_lseek -- reposition the read/write offset of an open file descriptor.
 *
 * whence:
 *   VFS_SEEK_SET (0): new offset = offset
 *   VFS_SEEK_CUR (1): new offset = current + offset
 *   VFS_SEEK_END (2): new offset = file_size + offset
 *
 * Returns the new absolute offset on success, -1 on error (invalid fd,
 * pipe fd, out-of-bounds offset, or unknown whence).
 */
int  vfs_lseek(int fd, int32_t offset, int whence);

/*
 * vfs_stat -- query metadata for the file or directory at `path`.
 *
 * Fills `out` with size and type.  Does not require an open fd.
 * Returns 0 on success, -1 if not found or driver has no stat support.
 */
int  vfs_stat(const char *path, vfs_stat_t *out);

/*
 * vfs_mkdir -- create a new empty directory at `path`.
 *
 * Returns 0 on success, -1 on failure (already exists, disk full, or
 * driver has no mkdir support).
 */
int  vfs_mkdir(const char *path);

/*
 * vfs_rename -- rename the file or directory at `oldpath` to `newpath`.
 *
 * Returns 0 on success, -1 on failure (not found, invalid names, or
 * driver has no rename support).
 */
int  vfs_rename(const char *oldpath, const char *newpath);

/*
 * vfs_chdir -- change the kernel working directory to `path`.
 *
 * If the filesystem has a stat op the path is validated as an existing
 * directory; otherwise the CWD string is updated unconditionally.
 * The CWD is kernel-global (shared by all processes in the current
 * single-mount design).  Returns 0 on success, -1 on failure.
 */
int  vfs_chdir(const char *path);

/*
 * vfs_getcwd -- copy the current working directory string into `buf`.
 *
 * At most `len`-1 bytes are copied; the result is always NUL-terminated.
 * Returns 0 on success, -1 if buf is NULL or len is 0.
 */
int  vfs_getcwd(char *buf, uint32_t len);

/*
 * vfs_pipe -- create an anonymous pipe and return two file descriptors.
 *
 * fds[0] is the read end; fds[1] is the write end.
 * Returns 0 on success, -1 on failure (fd table full or pipe pool full).
 *
 * Kernel build only: returns -1 in the host build.
 */
int  vfs_pipe(int fds[2]);

#endif /* _KERNEL_VFS_H */
