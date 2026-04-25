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

        /* ── Parse optional flags ───────────────────────────────────── */
        int flag_left  = 0;   /* '-': left-align within field width */
        int flag_zero  = 0;   /* '0': zero-pad (numeric only) */
        int flag_plus  = 0;   /* '+': always show sign */
        int flag_space = 0;   /* ' ': space before positive numbers */
        for (;;) {
            if      (*fmt == '-') { flag_left  = 1; fmt++; }
            else if (*fmt == '0') { flag_zero  = 1; fmt++; }
            else if (*fmt == '+') { flag_plus  = 1; fmt++; }
            else if (*fmt == ' ') { flag_space = 1; fmt++; }
            else break;
        }
        (void)flag_plus; (void)flag_space;   /* not yet used */

        /* ── Parse optional field width ─────────────────────────────── */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        /* ── Emit a field with optional padding ─────────────────────── */
#define EMIT_PADDED(str, len) do {                                     \
    int _n = (len);                                                    \
    int _w = (width > _n) ? width - _n : 0;                           \
    char _pad = (flag_zero && !flag_left) ? '0' : ' ';                \
    if (!flag_left)                                                    \
        for (int _i = 0; _i < _w && pos < (int)(sizeof(buf)-1); _i++) \
            buf[pos++] = _pad;                                         \
    for (int _i = 0; _i < _n && pos < (int)(sizeof(buf)-1); _i++)     \
        buf[pos++] = (str)[_i];                                        \
    if (flag_left)                                                     \
        for (int _i = 0; _i < _w && pos < (int)(sizeof(buf)-1); _i++) \
            buf[pos++] = ' ';                                          \
} while (0)

        switch (*fmt++) {
        case 'd': {
            int v = va_arg(ap, int);
            int neg = (v < 0);
            int n = uint_to_str(tmp, neg ? (unsigned int)-v : (unsigned int)v, 10);
            if (neg) {
                /* prepend '-' by shifting tmp */
                for (int i = n; i >= 0; i--) tmp[i+1] = tmp[i];
                tmp[0] = '-';
                n++;
            }
            EMIT_PADDED(tmp, n);
            break;
        }
        case 'u': {
            unsigned int v = va_arg(ap, unsigned int);
            int n = uint_to_str(tmp, v, 10);
            EMIT_PADDED(tmp, n);
            break;
        }
        case 'x': {
            unsigned int v = va_arg(ap, unsigned int);
            int n = uint_to_str(tmp, v, 16);
            EMIT_PADDED(tmp, n);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int n = 0;
            while (s[n]) n++;
            EMIT_PADDED(s, n);
            break;
        }
        case 'c': {
            int c = va_arg(ap, int);
            tmp[0] = (char)c;
            EMIT_PADDED(tmp, 1);
            break;
        }
        case '%':
            buf[pos++] = '%';
            break;
        default:
            buf[pos++] = '?';
            break;
        }
#undef EMIT_PADDED
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
