/*
 * Quilon user-space libc -- dirent.h
 *
 * Directory entry struct returned by readdir().  Layout must exactly
 * match vfs_dirent_t in kernel/include/kernel/vfs.h because SYS_READDIR
 * writes a vfs_dirent_t directly to the user-provided pointer.
 *
 * kernel vfs_dirent_t layout:
 *   char     name[VFS_NAME_MAX+1]  (VFS_NAME_MAX=63 -> 64 bytes)
 *   uint32_t size
 *   uint8_t  type
 */

#ifndef _DIRENT_H
#define _DIRENT_H

/* Must equal VFS_NAME_MAX in kernel/include/kernel/vfs.h -- the kernel writes
 * a vfs_dirent_t directly into this struct.  Raised from 12 (FAT16's 8.3
 * limit) for ext2's long names in section 16.2. */
#define DIRENT_NAME_MAX  63
#define DIRENT_TYPE_FILE  0
#define DIRENT_TYPE_DIR   1

typedef struct {
    char          name[DIRENT_NAME_MAX + 1]; /* NUL-terminated name        */
    unsigned int  size;                       /* file size in bytes         */
    unsigned char type;                       /* DIRENT_TYPE_FILE/DIR       */
} dirent_t;

#endif /* _DIRENT_H */
