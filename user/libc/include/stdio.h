/*
 * Quilon user-space libc -- stdio.h
 *
 * printf and puts are implemented on top of the write() syscall.
 * There is no FILE abstraction -- all output goes to stdout (fd 1).
 */

#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>

#define EOF (-1)

int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);

#endif /* _STDIO_H */
