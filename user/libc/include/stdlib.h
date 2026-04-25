/*
 * Quilon user-space libc — stdlib.h
 *
 * malloc/free are implemented on top of the sbrk() syscall using a simple
 * first-fit block allocator.  The heap starts at USER_HEAP_START (0x800000)
 * on the first allocation.
 */

#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

void  *malloc(size_t size);
void   free(void *ptr);
int    atoi(const char *s);
void   exit(int code) __attribute__((noreturn));

#ifndef NULL
#define NULL ((void*)0)
#endif

#endif /* _STDLIB_H */
