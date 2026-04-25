/*
 * Quilon user-space libc — stdio.c
 *
 * printf and friends built on top of the write() syscall.
 *
 * Design: vprintf() formats into a 512-byte stack buffer then issues a
 * single write(STDOUT_FILENO, buf, len) syscall.  No FILE objects, no
 * buffering layer — correct for a single-process demo shell.
 *
 * Supported format specifiers:
 *   %d  — signed decimal integer
 *   %u  — unsigned decimal integer
 *   %x  — unsigned hexadecimal (lowercase)
 *   %s  — NUL-terminated string
 *   %c  — single character
 *   %%  — literal percent sign
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ── Internal helpers ─────────────────────────────────────────────────── */

/* Convert unsigned int to decimal/hex string; return length written. */
static int uint_to_str(char *out, unsigned int v, int base)
{
    const char *digits = "0123456789abcdef";
    char tmp[12];
    int  i = 0;

    if (v == 0) {
        out[0] = '0';
        out[1] = '\0';
        return 1;
    }
    while (v > 0) {
        tmp[i++] = digits[v % (unsigned)base];
        v /= (unsigned)base;
    }
    /* reverse into out */
    for (int j = 0; j < i; j++)
        out[j] = tmp[i - 1 - j];
    out[i] = '\0';
    return i;
}

/* ── Public API ───────────────────────────────────────────────────────── */

int vprintf(const char *fmt, va_list ap)
{
    char     buf[512];
    int      pos = 0;
    char     tmp[16];

    while (*fmt && pos < (int)(sizeof(buf) - 1)) {
        if (*fmt != '%') {
            buf[pos++] = *fmt++;
            continue;
        }
        fmt++;   /* skip '%' */

        switch (*fmt++) {
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) {
                buf[pos++] = '-';
                v = -v;
            }
            int n = uint_to_str(tmp, (unsigned int)v, 10);
            for (int i = 0; i < n && pos < (int)(sizeof(buf) - 1); i++)
                buf[pos++] = tmp[i];
            break;
        }
        case 'u': {
            unsigned int v = va_arg(ap, unsigned int);
            int n = uint_to_str(tmp, v, 10);
            for (int i = 0; i < n && pos < (int)(sizeof(buf) - 1); i++)
                buf[pos++] = tmp[i];
            break;
        }
        case 'x': {
            unsigned int v = va_arg(ap, unsigned int);
            int n = uint_to_str(tmp, v, 16);
            for (int i = 0; i < n && pos < (int)(sizeof(buf) - 1); i++)
                buf[pos++] = tmp[i];
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && pos < (int)(sizeof(buf) - 1))
                buf[pos++] = *s++;
            break;
        }
        case 'c': {
            int c = va_arg(ap, int);
            buf[pos++] = (char)c;
            break;
        }
        case '%':
            buf[pos++] = '%';
            break;
        default:
            buf[pos++] = '?';
            break;
        }
    }

    if (pos > 0)
        write(STDOUT_FILENO, buf, pos);

    return pos;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s)
{
    int len = (int)strlen(s);
    write(STDOUT_FILENO, s, len);
    write(STDOUT_FILENO, "\n", 1);
    return 0;
}

int putchar(int c)
{
    char ch = (char)c;
    write(STDOUT_FILENO, &ch, 1);
    return c;
}
