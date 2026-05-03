#ifndef _KERNEL_PAGING_H
#define _KERNEL_PAGING_H

#include <stdint.h>

/* ── Page-entry flag bits (low 12 bits of every PDE / PTE) ─────────────── */
#define PAGE_PRESENT   (1u << 0)   /* P   – page is present in RAM         */
#define PAGE_WRITABLE  (1u << 1)   /* R/W – allow writes                   */
#define PAGE_USER      (1u << 2)   /* U/S – accessible from ring 3         */

/* ── Higher-half kernel constants (section 9.1) ────────────────────────────
 *
 * The kernel is linked at virtual 0xC0100000 but loaded at physical 0x100000.
 * KERNEL_OFFSET is the difference: virtual - physical = 0xC0000000.
 * KERNEL_PD_IDX is the page directory index for 0xC0000000 (= 0xC0000000 >> 22 = 768).
 *
 * Physical address of a kernel virtual: phys = virt - KERNEL_OFFSET
 * Virtual address of a physical page:   virt = phys + KERNEL_OFFSET
 *   (only valid for pages in the first 4 MiB, covered by the kernel mapping)
 */
#define KERNEL_OFFSET    0xC0000000u
#define KERNEL_PD_IDX    (KERNEL_OFFSET >> 22)   /* = 768 */

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
/* ── Kernel page directory ──────────────────────────────────────────────────
 * Exposed so shell_cmd_exec can restore it after running a process in its
 * own address space.
 */
extern uint32_t page_directory[1024];

/* Return a pointer to the kernel's global page directory.
 * Used when switching back to the kernel address space after exec_longjmp. */
uint32_t *paging_get_kernel_pd(void);

/* Return the physical address of the kernel page directory for loading into CR3.
 * Use this (not paging_get_kernel_pd()) when switching to the kernel address space. */
uint32_t paging_kernel_cr3(void);

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

/* Like paging_map_page, but allocates a page table via pmm_alloc_page() if
 * no page table exists yet for the 4-MiB PD slot covering virt.
 *
 * Use this when mapping ELF segments or other memory outside the first 4 MiB
 * that was set up by paging_initialize().
 *
 * Returns 0 on success, -1 if pmm_alloc_page() returns NULL (out of memory).
 *
 * Side effect: the newly allocated page table page is zero-initialised (all
 * PTEs marked not-present) before the first entry is written.
 */
int paging_map_page_alloc(uint32_t virt, uint32_t phys, uint32_t flags);

/* ── Per-process address space (section 5.1) ────────────────────────────── */

/*
 * paging_create_address_space — allocate a fresh page directory for a process.
 *
 * Allocates a new 4-KiB page directory and copies the kernel's PD entry 0
 * (the identity-mapped first 4-MiB page table) into it.  The kernel half is
 * shared (same physical page table), so kernel code and data are visible in
 * every address space.  User pages are private to each process.
 *
 * Returns the new page directory pointer (phys == virt, identity-mapped),
 * or NULL on OOM.  Cast to uint32_t for paging_switch().
 */
uint32_t *paging_create_address_space(void);

/*
 * paging_switch — load pd_phys into CR3, switching the active address space.
 *
 * Flushes the TLB as a side effect (CR3 write always does this on x86).
 * Call with the physical address of a page directory (returned by
 * paging_create_address_space() or paging_get_kernel_pd()).
 */
void paging_switch(uint32_t pd_phys);

/*
 * paging_fork_address_space — copy all user pages from parent_pd to child_pd.
 *
 * Implements the memory-duplication step of fork() (section 6.2).  Walks
 * every page directory entry in parent_pd EXCEPT entry 0 (the shared kernel
 * page table, already installed in child_pd by paging_create_address_space).
 *
 * For each present PDE in [1, 1023]:
 *   1. Allocate a new page table for child_pd.
 *   2. For each present PTE in that page table:
 *       a. Allocate a fresh physical page.
 *       b. Copy the 4 KiB content (parent phys addr == parent virt addr
 *          because all PMM pages are identity-mapped in the first 4 MiB).
 *       c. Map it in child_pd at the same virtual address with the same flags.
 *
 * No TLB flush is needed: child_pd is not yet loaded in CR3.
 *
 * Returns 0 on success, -1 if pmm_alloc_page() fails (OOM).
 * On failure, any pages already copied are leaked (acceptable for now —
 * a production OS would free them on rollback).
 */
int paging_fork_address_space(uint32_t *parent_pd, uint32_t *child_pd);

/*
 * paging_map_page_alloc_into — map a page into an arbitrary page directory.
 *
 * Like paging_map_page_alloc() but operates on an explicit pd[] instead of
 * the active global page_directory[].  Used by elf_load_into() to populate
 * a child process's address space before it is first scheduled.
 *
 * pd   — pointer to the target page directory (must be identity-mapped, i.e.
 *         in the first 4 MiB, as all pmm_alloc_page() results are).
 * virt — virtual address to map.
 * phys — physical page to map there.
 * flags — PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER as needed.
 *
 * Returns 0 on success, -1 if pmm_alloc_page() fails for a new page table.
 * No TLB flush — the target PD is not currently active in CR3.
 */
int paging_map_page_alloc_into(uint32_t *pd, uint32_t virt,
                                uint32_t phys, uint32_t flags);

#endif /* _KERNEL_PAGING_H */
