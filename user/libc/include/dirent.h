/*
 * Quilon user-space libc -- dirent.h
 *
 * Directory entry struct returned by readdir().  Layout must exactly
 * match vfs_dirent_t in kernel/include/kernel/vfs.h because SYS_READDIR
 * writes a vfs_dirent_t directly to the user-provided pointer.
 *
 * kernel vfs_dirent_t layout:
 *   char     name[VFS_NAME_MAX+1]  (VFS_NAME_MAX=12 -> 13 bytes)
 *   uint32_t size
 *   uint8_t  type
 */

#ifndef _DIRENT_H
#define _DIRENT_H

#define DIRENT_NAME_MAX  12    /* 8.3 name: 8 chars + '.' + 3 chars + NUL */
#define DIRENT_TYPE_FILE  0
#define DIRENT_TYPE_DIR   1

typedef struct {
    char          name[DIRENT_NAME_MAX + 1]; /* NUL-terminated 8.3 name   */
    unsigned int  size;                       /* file size in bytes         */
    unsigned char type;                       /* DIRENT_TYPE_FILE/DIR       */
} dirent_t;

#endif /* _DIRENT_H */
