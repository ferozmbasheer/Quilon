/*
 * Quilon OS -- File browser pure logic (ROADMAP3 18.5)
 *
 * Path joining ("..", "." and absolute names), what opening a name does
 * (directory / program / text), and the listing order.  No syscalls --
 * host-tested in tests/test_widgets.c.
 */

#ifndef _USER_FILEBR_FB_PATH_H
#define _USER_FILEBR_FB_PATH_H

#define FB_PATH_MAX   256
#define FB_NAME_MAX    64

typedef struct {
    char name[FB_NAME_MAX];
    int  is_dir;
} fb_entry_t;

static inline int fb_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static inline void fb_copy(char *d, const char *s)
{
    while ((*d++ = *s++) != '\0') { }
}

static inline char fb_lower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Case-insensitive compare (FAT names are upper case, ext2's usually not). */
static inline int fb_strcasecmp(const char *a, const char *b)
{
    while (*a && fb_lower(*a) == fb_lower(*b)) { a++; b++; }
    return (unsigned char)fb_lower(*a) - (unsigned char)fb_lower(*b);
}

/* Drop the last component: "/a/b" -> "/a", "/a" -> "/", "/" -> "/". */
static inline void fb_parent(char *path)
{
    int n = fb_strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;        /* trailing slashes */
    while (n > 0 && path[n - 1] != '/') n--;        /* the name */
    while (n > 1 && path[n - 1] == '/') n--;        /* its separator */
    if (n == 0) { path[0] = '/'; n = 1; }
    path[n] = '\0';
}

/*
 * out = `name` taken relative to directory `dir` (absolute, normalised:
 * no trailing '/' except the root).  `name` may be absolute or a relative
 * path with "." / ".." components.  0 = OK, -1 = too long (out untouched).
 */
static inline int fb_path_join(const char *dir, const char *name, char *out)
{
    char tmp[FB_PATH_MAX];
    int  n = 0;
    if (name[0] == '/') { tmp[0] = '/'; tmp[1] = '\0'; n = 1; }
    else {
        n = fb_strlen(dir);
        if (n >= FB_PATH_MAX) return -1;
        for (int i = 0; i <= n; i++) tmp[i] = dir[i];
        if (n == 0) { tmp[0] = '/'; tmp[1] = '\0'; n = 1; }
    }

    const char *p = name;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        int len = 0;
        while (p[len] && p[len] != '/') len++;
        if (len == 1 && p[0] == '.') {
            /* stay */
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            fb_parent(tmp);
            n = fb_strlen(tmp);
        } else {
            int sep = (n > 1);                       /* not after the root */
            if (n + sep + len >= FB_PATH_MAX) return -1;
            if (sep) tmp[n++] = '/';
            for (int i = 0; i < len; i++) tmp[n++] = p[i];
            tmp[n] = '\0';
        }
        p += len;
    }
    for (int i = 0; i <= n; i++) out[i] = tmp[i];
    return 0;
}

/* What opening an entry does. */
enum { FB_OPEN_DIR = 0, FB_OPEN_PROG, FB_OPEN_TEXT };

static inline int fb_open_kind(const char *name, int is_dir)
{
    if (is_dir) return FB_OPEN_DIR;
    int n = fb_strlen(name);
    if (n > 4 && fb_strcasecmp(name + n - 4, ".elf") == 0) return FB_OPEN_PROG;
    return FB_OPEN_TEXT;
}

/* Listing order: ".." first, then directories, then files, each A-Z
 * ignoring case.  Insertion sort -- a directory is a few dozen entries. */
static inline int fb_entry_before(const fb_entry_t *a, const fb_entry_t *b)
{
    int au = (a->name[0] == '.' && a->name[1] == '.' && !a->name[2]);
    int bu = (b->name[0] == '.' && b->name[1] == '.' && !b->name[2]);
    if (au != bu) return au;
    if (a->is_dir != b->is_dir) return a->is_dir;
    return fb_strcasecmp(a->name, b->name) < 0;
}

static inline void fb_sort(fb_entry_t *e, int n)
{
    for (int i = 1; i < n; i++) {
        fb_entry_t k = e[i];
        int j = i - 1;
        while (j >= 0 && fb_entry_before(&k, &e[j])) { e[j + 1] = e[j]; j--; }
        e[j + 1] = k;
    }
}

#endif /* _USER_FILEBR_FB_PATH_H */
