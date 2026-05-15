#ifndef _KERNEL_INITRD_H
#define _KERNEL_INITRD_H

#include <stdint.h>
#include <kernel/vfs.h>

/* Maximum filename length (16-byte header field: 15 chars + NUL). */
#define INITRD_NAME_MAX  15

/* Maximum number of files per image. */
#define INITRD_MAX_FILES 32

/*
 * initrd_entry_t -- one parsed file record.
 *
 * `data` points directly into the raw image buffer; no copying is done.
 * The image buffer must remain valid for the lifetime of the mount.
 */
typedef struct {
    char      name[16];   /* NUL-terminated filename */
    uint32_t  size;       /* file data size in bytes */
    uint8_t  *data;       /* pointer into the raw initrd image */
} initrd_entry_t;

/*
 * initrd_ctx_t -- driver context; one per mounted initrd image.
 * Pass to vfs_mount() as the `ctx` argument.
 */
typedef struct {
    uint8_t       *base;                       /* start of raw image */
    uint32_t       image_size;                 /* total image size in bytes */
    uint32_t       file_count;                 /* number of files parsed */
    initrd_entry_t files[INITRD_MAX_FILES];    /* parsed file table */
} initrd_ctx_t;

/* VFS ops table for the initrd driver.  Pass to vfs_mount(). */
extern const vfs_ops_t initrd_vfs_ops;

/*
 * initrd_mount -- parse a raw initrd image and fill `ctx`.
 *
 * Image format (all integers little-endian):
 *   [ uint32_t  file_count N                                  ]
 *   [ N × { char name[16]; uint32_t data_size; uint8_t data[] } ]
 *
 * Returns 0 on success, -1 if the image is NULL, too small, has more files
 * than INITRD_MAX_FILES, or if any file record extends past the image end.
 */
int initrd_mount(initrd_ctx_t *ctx, void *addr, uint32_t size);

/*
 * initrd_build_demo -- write a 3-file demo initrd image into `buf`.
 *
 * `buf` must be at least 256 bytes.  Returns the image size in bytes, or 0
 * if `bufsize` is too small.  Used by the kernel boot demo and unit tests.
 */
uint32_t initrd_build_demo(uint8_t *buf, uint32_t bufsize);

#endif /* _KERNEL_INITRD_H */
