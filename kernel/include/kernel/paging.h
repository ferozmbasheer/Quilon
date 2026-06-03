#ifndef _KERNEL_PAGING_H
#define _KERNEL_PAGING_H

#include <stdint.h>

/* -- Page-entry flag bits (low 12 bits of every PDE / PTE) --------------- */
#define PAGE_PRESENT   (1u << 0)   /* P   – page is present in RAM         */
#define PAGE_WRITABLE  (1u << 1)   /* R/W – allow writes                   */
#define PAGE_USER      (1u << 2)   /* U/S – accessible from ring 3         */

/* Bit 3 (PWT) and bit 4 (PCD) control caching behaviour.
 * PAGE_NOCACHE sets PCD (Page Cache Disable) to bypass the CPU cache for
 * MMIO regions.  Always set this on page-table entries that map device
 * registers (e.g. the Local APIC at 0xFEE00000).                          */
#define PAGE_NOCACHE   (1u << 4)

/* Bit 9 is available to the OS (x86 reserved for software use).
 * We use it to mark copy-on-write pages (section 9.3):
 *   – Set on both parent and child PTEs by paging_fork_address_space().
 *   – Cleared and PAGE_WRITABLE restored by paging_cow_handle() when the
 *     first write fault is taken against this page. */
#define PAGE_COW       (1u << 9)

/* -- Higher-half kernel constants (section 9.1) ----------------------------
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

/* -- Virtual address decomposition -----------------------------------------
 *  31..22  page directory index  (10 bits) -> selects one of 1024 PDEs
 *  21..12  page table index      (10 bits) -> selects one of 1024 PTEs
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
/* -- Kernel page directory --------------------------------------------------
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

/* Map a hardware MMIO region: like paging_map_page_alloc but always sets
 * PAGE_NOCACHE so the CPU does not cache MMIO register reads/writes.
 * Used by apic_initialize() to map the Local APIC at 0xFEE00000. */
void paging_map_mmio(uint32_t virt, uint32_t phys);

/* -- kmap: transient kernel access to an arbitrary physical page ----------- *
 *
 * The kernel can dereference a physical address directly only within the first
 * 4 MiB identity map.  Data pages (heap, ELF segments, stack, CoW copies) are
 * allocated above 4 MiB, so the kernel uses kmap() to obtain a temporary VA for
 * one such page and kunmap() to release it.  The mapping window is shared into
 * every address space, so kmap works under any CR3 (including the page-fault
 * handler running under a user process).  Returns NULL if all slots are busy.
 */
void *kmap(uint32_t phys);
void  kunmap(void *ptr);

/* Allocate a zeroed data page above the identity map (falls back to low memory
 * if high memory is exhausted).  Returns the physical address, or NULL on OOM.
 * Use for pages the kernel only needs transient access to (heap/ELF/stack/CoW);
 * page tables and page directories must still use pmm_alloc_page() so they stay
 * reachable through the identity map. */
void *pmm_alloc_data_page(void);

/* -- Per-process address space (section 5.1) ------------------------------ */

/*
 * paging_create_address_space -- allocate a fresh page directory for a process.
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
 * paging_switch -- load pd_phys into CR3, switching the active address space.
 *
 * Flushes the TLB as a side effect (CR3 write always does this on x86).
 * Call with the physical address of a page directory (returned by
 * paging_create_address_space() or paging_get_kernel_pd()).
 */
void paging_switch(uint32_t pd_phys);

/*
 * paging_fork_address_space -- CoW fork of all user pages (section 9.3).
 *
 * Implements copy-on-write fork.  Walks every page directory entry in
 * parent_pd EXCEPT the shared kernel entries (PD[0] and PD[KERNEL_PD_IDX]).
 *
 * For each present PDE in [1, 1023] (excluding KERNEL_PD_IDX):
 *   1. Allocate a new page table for child_pd.
 *   2. For each present PTE in that page table:
 *       a. Call pmm_ref_page() to record that a second PTE references
 *          the same physical page.
 *       b. If the page is writable: clear PAGE_WRITABLE and set PAGE_COW
 *          in BOTH the parent PTE and the child PTE.  Both processes will
 *          now fault on the first write.
 *       c. If the page is read-only: share it unchanged (no CoW needed).
 *
 * The caller MUST flush the parent TLB after this call (e.g. by calling
 * paging_switch(current_process->cr3)) because we modified parent PTEs.
 *
 * No TLB flush is needed for child_pd: it is not yet loaded in CR3.
 *
 * Returns 0 on success, -1 if pmm_alloc_page() fails (OOM).
 */
int paging_fork_address_space(uint32_t *parent_pd, uint32_t *child_pd);

/*
 * paging_cow_handle -- resolve a copy-on-write write fault (section 9.3).
 *
 * Called by the page fault handler when a write to a PAGE_COW page raises
 * a protection fault (err_code bit 1 set, page was present but read-only).
 *
 * Algorithm:
 *   1. Locate the PTE for fault_addr in pd[].
 *   2. Verify PAGE_COW is set (otherwise return -1 -- not a CoW fault).
 *   3. If pmm_page_refcount() == 1: this process is the sole remaining
 *      owner -- clear PAGE_COW, restore PAGE_WRITABLE, no copy needed.
 *   4. If refcount > 1: allocate a new physical page, copy content,
 *      call pmm_free_page() to decrement the shared page's refcount,
 *      install the new page as writable (PAGE_COW cleared).
 *   5. invlpg the fault address to invalidate the stale TLB entry.
 *
 * Returns 0 on success (fault handled, CPU will re-execute), -1 on
 * failure (not a CoW fault or OOM -- caller should send SIGSEGV).
 */
int paging_cow_handle(uint32_t *pd, uint32_t fault_addr);

/*
 * paging_map_page_alloc_into -- map a page into an arbitrary page directory.
 *
 * Like paging_map_page_alloc() but operates on an explicit pd[] instead of
 * the active global page_directory[].  Used by elf_load_into() to populate
 * a child process's address space before it is first scheduled.
 *
 * pd   -- pointer to the target page directory (must be identity-mapped, i.e.
 *         in the first 4 MiB, as all pmm_alloc_page() results are).
 * virt -- virtual address to map.
 * phys -- physical page to map there.
 * flags -- PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER as needed.
 *
 * Returns 0 on success, -1 if pmm_alloc_page() fails for a new page table.
 * No TLB flush -- the target PD is not currently active in CR3.
 */
int paging_map_page_alloc_into(uint32_t *pd, uint32_t virt,
                                uint32_t phys, uint32_t flags);

#endif /* _KERNEL_PAGING_H */
