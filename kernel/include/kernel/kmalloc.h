#ifndef _KERNEL_KMALLOC_H
#define _KERNEL_KMALLOC_H

#include <stddef.h>
#include <stdint.h>

/* ── Kernel heap allocator ───────────────────────────────────────────────────
 *
 * A simple first-fit linked-list allocator.  Every allocation begins with a
 * block_header_t that records the usable size, a magic canary, a free flag,
 * and a pointer to the next header.  On kfree(), adjacent free blocks are
 * coalesced to keep fragmentation low.
 *
 * The heap lives in the 1 MiB of physical RAM immediately after kernel_end,
 * which is already identity-mapped by paging_initialize().
 *
 * Call order: gdt → idt → terminal → pmm → paging → kmalloc_initialize.
 */

/* Initial heap size in bytes (1 MiB — fits comfortably inside the first 4 MiB
 * identity-mapped region).                                                    */
#define HEAP_SIZE (1024u * 1024u)

/* Canary stored in every block header.  Used by kfree() to detect corruption
 * (e.g. double-free or write past the end of an allocation).                  */
#define HEAP_MAGIC 0xDEADBEEFu

/* Set up the heap.  Must be called once before any kmalloc/kfree call.
 * Panics (via kprintf + hlt) if the heap would overflow the 4 MiB identity-
 * mapped region.                                                               */
void  kmalloc_initialize(void);

/* Allocate at least `size` bytes from the kernel heap.
 * Returns a pointer aligned to 8 bytes, or NULL if the heap is full.         */
void *kmalloc(size_t size);

/* Release a block previously returned by kmalloc.
 * Passing NULL is a no-op.  Passing any other invalid pointer (wrong magic)
 * prints a warning and returns without touching memory.                        */
void  kfree(void *ptr);

/* Print a one-line summary of every block to the terminal.
 * Useful when debugging allocator bugs.                                        */
void  kmalloc_dump(void);

#endif /* _KERNEL_KMALLOC_H */
