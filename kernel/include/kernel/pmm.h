#ifndef _KERNEL_PMM_H
#define _KERNEL_PMM_H

#include <stdint.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* Call once from kernel_main after terminal_initialize so errors are visible. */
void     pmm_initialize(void *multiboot_info);

/* Returns a 4 KiB-aligned physical address, or NULL if out of memory. */
void    *pmm_alloc_page(void);

/*
 * Like pmm_alloc_page() but only returns pages whose physical address is
 * >= 4 MiB (i.e., not in the identity-mapped first 4 MiB).  Use this for
 * large buffers (e.g. shadow framebuffer) that are only ever accessed via
 * virtual addresses so that sub-4 MiB pages remain available for paging
 * structures (page directories and page tables) which require identity-map
 * access.  Returns NULL if no above-4 MiB pages are free.
 */
void    *pmm_alloc_page_above_4mib(void);

/* addr must be a value previously returned by pmm_alloc_page. */
void     pmm_free_page(void *addr);

/* Number of pages currently free. */
uint32_t pmm_free_page_count(void);

/* ── Reference counting for Copy-on-Write (section 9.3) ─────────────────────
 *
 * Every physical page has a reference count.  pmm_alloc_page() sets it to 1.
 * pmm_ref_page() increments it when a page is shared (CoW fork).
 * pmm_free_page() decrements it and only releases the page when it reaches 0.
 *
 * This allows multiple page table entries to point to the same physical page
 * without double-freeing it.
 */

/* Increment the reference count of a previously-allocated page.
 * Used by paging_fork_address_space() when sharing a physical page
 * between parent and child during a CoW fork. */
void    pmm_ref_page(void *addr);

/* Return the current reference count of a physical page.
 * Used by paging_cow_handle() to decide whether to copy or just remap. */
uint8_t pmm_page_refcount(void *addr);

/* ── Test / internal interface ───────────────────────────────────────────────
 * Bypasses multiboot parsing. Resets the bitmap, marks [free_base,
 * free_base+free_len) as available, then re-reserves the null page and
 * [reserved_base, reserved_base+reserved_len) as the "kernel" region.
 * Pass reserved_len = 0 to skip the kernel reservation.
 * Used by host-side unit tests to avoid 32-bit/64-bit pointer issues.    */
void pmm_init_range(uint32_t free_base,     uint32_t free_len,
                    uint32_t reserved_base, uint32_t reserved_len);

#endif /* _KERNEL_PMM_H */
