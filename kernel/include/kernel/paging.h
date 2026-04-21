#ifndef _KERNEL_PAGING_H
#define _KERNEL_PAGING_H

#include <stdint.h>

/* ── Page-entry flag bits (low 12 bits of every PDE / PTE) ─────────────── */
#define PAGE_PRESENT   (1u << 0)   /* P   – page is present in RAM         */
#define PAGE_WRITABLE  (1u << 1)   /* R/W – allow writes                   */
#define PAGE_USER      (1u << 2)   /* U/S – accessible from ring 3         */

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096u
#endif

/* ── Virtual address decomposition ─────────────────────────────────────────
 *  31..22  page directory index  (10 bits) → selects one of 1024 PDEs
 *  21..12  page table index      (10 bits) → selects one of 1024 PTEs
 *  11..0   byte offset within the page     (12 bits)
 */
#define VIRT_PD_INDEX(v)  (((uint32_t)(v)) >> 22)
#define VIRT_PT_INDEX(v)  ((((uint32_t)(v)) >> 12) & 0x3FFu)
#define VIRT_OFFSET(v)    (((uint32_t)(v)) & 0xFFFu)

/* Build a page directory entry or page table entry.
 *
 * phys_addr – 4 KiB-aligned physical address; low 12 bits are masked out.
 * flags     – combination of PAGE_PRESENT, PAGE_WRITABLE, PAGE_USER, …
 *
 * This is a static inline so the host-side unit tests can use it without
 * pulling in any x86 assembly.
 */
static inline uint32_t paging_make_entry(uint32_t phys_addr, uint32_t flags)
{
    return (phys_addr & ~(uint32_t)0xFFF) | (flags & 0xFFFu);
}

/* Initialise paging:
 *   1. Identity-map the first 4 MiB (keeps the kernel accessible after
 *      the MMU is turned on, since virtual == physical for that range).
 *   2. Load the page directory into CR3.
 *   3. Set bit 31 (PG) in CR0 to enable the MMU.
 *
 * Call after pmm_initialize() so the PMM bitmap is already populated.
 */
void paging_initialize(void);

/* Mark every PTE in [virt_start, virt_end) that is already present with
 * the PAGE_USER flag (and also set PAGE_USER on the covering PDEs).
 * Pages that are not yet mapped are silently skipped.
 * Flushes the TLB by reloading CR3 after the update.
 *
 * Used by usermode_initialize() to allow ring-3 code to access the
 * identity-mapped first 4 MiB.
 */
void paging_set_user_access(uint32_t virt_start, uint32_t virt_end);

/* Map a single physical page at virtual address virt with the given flags.
 *
 * Requires that a page table already exists for the PD slot covering virt
 * (i.e. that paging_initialize() has already mapped something in the same
 * 4-MiB region).  Returns 0 on success, -1 if no page table exists for
 * that slot.
 *
 * Invalidates the single TLB entry for virt via invlpg.
 */
int paging_map_page(uint32_t virt, uint32_t phys, uint32_t flags);

#endif /* _KERNEL_PAGING_H */
