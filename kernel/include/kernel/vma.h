/*
 * Quilon OS — Virtual Memory Area (VMA) — section 9.2
 *
 * A VMA (Virtual Memory Area) is the kernel's record of one contiguous,
 * logically coherent region of a process's virtual address space.  Every
 * valid address in a process must belong to exactly one VMA; any access to
 * an address not covered by any VMA is a genuine segfault.
 *
 * Background
 * ──────────
 * Before demand paging, elf_load_into() allocated physical pages for every
 * byte of every ELF segment up front.  With demand paging, the kernel only
 * records that a range is valid (via a VMA), but defers the physical-page
 * allocation until the process actually touches that address.
 *
 * How it fits together:
 *
 *   1.  elf_load_into() creates VMAs for each PT_LOAD segment and the stack.
 *       It still maps pages for file-data regions eagerly, but leaves
 *       purely-BSS pages (p_memsz > p_filesz) unmapped — those are covered
 *       by the VMA and will be zero-filled on demand.
 *
 *   2.  sbrk() (SYS_SBRK) creates / extends a heap VMA instead of
 *       allocating physical pages.  Physical pages arrive page-fault by page-
 *       fault as the user program writes to heap memory.
 *
 *   3.  The page-fault handler (vector 14 in exceptions.c):
 *         a.  If the page is already present:  protection fault → SIGSEGV.
 *         b.  If the page is not present:
 *               – Look up the fault address in the process's VMA list.
 *               – No VMA → genuine segfault → SIGSEGV.
 *               – VMA found → allocate a zero physical page, map it with the
 *                 VMA's permissions, and return.  The CPU re-executes the
 *                 faulting instruction transparently.
 *
 * VMA flags
 * ─────────
 * VMA_R / VMA_W / VMA_X describe permissions.  These mirror the ELF PF_*
 * segment flags but are stored separately so they are available without
 * re-parsing the ELF binary.
 *
 * VMA_ANON means the region is anonymous — there is no file backing it.
 * Zero-fill is the correct behaviour on a demand-page fault.
 *
 * VMA_STACK marks the user stack, which is special only in that we know it
 * grows downward.  The fault handler treats it identically (zero-fill a page
 * on any not-present fault within the VMA range); the flag is informational.
 *
 * Memory layout example for a typical ELF process:
 *
 *   VMA 0: [0x401000, 0x402000)  VMA_R|VMA_X        — .text (file-backed, eagerly mapped)
 *   VMA 1: [0x402000, 0x403000)  VMA_R|VMA_W|VMA_ANON — .bss (demand-paged)
 *   VMA 2: [0x800000, 0x801000)  VMA_R|VMA_W|VMA_ANON — heap (grows as sbrk extends end)
 *   VMA 3: [0xBC000000, 0xC0000000)  VMA_R|VMA_W|VMA_ANON|VMA_STACK — stack
 *            ^                  ^
 *            |                  USER_STACK_TOP (exclusive)
 *            USER_STACK_TOP - 64 pages (max stack depth)
 */

#ifndef _KERNEL_VMA_H
#define _KERNEL_VMA_H

#include <stdint.h>

/* Maximum number of VMAs per process. */
#define PROC_VMA_MAX   16

/* VMA permission / type flags */
#define VMA_R     (1u << 0)   /* region is readable                         */
#define VMA_W     (1u << 1)   /* region is writable                         */
#define VMA_X     (1u << 2)   /* region is executable                       */
#define VMA_ANON  (1u << 3)   /* anonymous (zero-fill on demand)             */
#define VMA_STACK (1u << 4)   /* user stack — grows downward                */

/*
 * vma_t — one Virtual Memory Area descriptor.
 *
 * start  — inclusive lower bound (page-aligned).
 * end    — exclusive upper bound (page-aligned).  start < end always.
 * flags  — combination of VMA_R, VMA_W, VMA_X, VMA_ANON, VMA_STACK.
 * used   — 1 if this slot is active, 0 if it is free.
 *
 * The page-fault handler maps demand pages with PAGE_USER | PAGE_PRESENT,
 * plus PAGE_WRITABLE if (flags & VMA_W).
 */
typedef struct {
    uint32_t start;
    uint32_t end;
    uint32_t flags;
    uint8_t  used;
} vma_t;

/*
 * vma_init — zero-initialise an array of VMA slots.
 *
 * Call once per process during process_create() to prepare an empty VMA table.
 */
void vma_init(vma_t *vmas, int count);

/*
 * vma_add — insert a new VMA [start, end) with the given flags.
 *
 * Returns 0 on success, -1 if no free slot is available (PROC_VMA_MAX reached).
 */
int vma_add(vma_t *vmas, int count, uint32_t start, uint32_t end, uint32_t flags);

/*
 * vma_find — return the VMA whose range covers addr (start <= addr < end).
 *
 * Returns a pointer into the vmas array, or NULL if no VMA covers addr.
 * Used by the page-fault handler to validate a faulting address.
 */
vma_t *vma_find(vma_t *vmas, int count, uint32_t addr);

/*
 * vma_find_start — return the VMA whose start address equals start exactly.
 *
 * Returns a pointer into the vmas array, or NULL if not found.
 * Used by sbrk to locate the heap VMA for extension.
 */
vma_t *vma_find_start(vma_t *vmas, int count, uint32_t start);

/*
 * vma_extend — update the end address of the VMA whose start == start.
 *
 * Sets vma->end = new_end.  new_end must be greater than the current end
 * (shrinking is not supported here — that is a future exercise).
 *
 * Returns 0 on success, -1 if no VMA with that start address exists.
 */
int vma_extend(vma_t *vmas, int count, uint32_t start, uint32_t new_end);

/*
 * vma_remove — mark the VMA whose start == start as unused.
 *
 * No-op if no such VMA exists.  The freed slot is immediately reusable
 * by vma_add().
 */
void vma_remove(vma_t *vmas, int count, uint32_t start);

#endif /* _KERNEL_VMA_H */
