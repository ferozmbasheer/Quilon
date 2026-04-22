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

/* addr must be a value previously returned by pmm_alloc_page. */
void     pmm_free_page(void *addr);

/* Number of pages currently free. */
uint32_t pmm_free_page_count(void);

/* ── Test / internal interface ───────────────────────────────────────────────
 * Bypasses multiboot parsing. Resets the bitmap, marks [free_base,
 * free_base+free_len) as available, then re-reserves the null page and
 * [reserved_base, reserved_base+reserved_len) as the "kernel" region.
 * Pass reserved_len = 0 to skip the kernel reservation.
 * Used by host-side unit tests to avoid 32-bit/64-bit pointer issues.    */
void pmm_init_range(uint32_t free_base,     uint32_t free_len,
                    uint32_t reserved_base, uint32_t reserved_len);

#endif /* _KERNEL_PMM_H */
