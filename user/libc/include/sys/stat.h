#ifndef _SYS_STAT_H
#define _SYS_STAT_H

/* Layout must match kernel vfs_stat_t exactly (SYS_STAT writes directly to this). */
typedef struct {
    unsigned int  st_size;   /* file size in bytes (0 for directories) */
    unsigned char st_type;   /* 0 = regular file, 1 = directory */
} stat_t;

#define S_ISREG(t) ((t) == 0)
#define S_ISDIR(t) ((t) == 1)

#endif /* _SYS_STAT_H */
