#include <stdint.h>
#include <string.h>

#include <kernel/paging.h>
#include <kernel/pmm.h>
#include <kernel/vma.h>
#include <kernel/spinlock.h>

/* Page directory and first page table: statically allocated and 4 KiB-
 * aligned so the CPU will accept them as PD/PT base addresses.
 * Living in .bss they are zero-initialised before kernel_main runs, so
 * every unset entry is already marked not-present (bit 0 = 0).          */
/* Not static: paging_create_address_space() and paging_get_kernel_pd()
 * need access to copy the kernel entries into new page directories.     */
uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t first_page_table[1024]  __attribute__((aligned(4096)));
static uint32_t second_page_table[1024] __attribute__((aligned(4096)));

/*
 * Static pool of page tables for kernel dynamic mappings (e.g. VBE framebuffer,
 * APIC MMIO).  paging_map_page_alloc uses these instead of PMM because the PT
 * must be accessible via its physical address (identity map).  By VBE init time
 * the sub-4 MiB PMM pool may be exhausted; a BSS PT is always within 4 MiB.
 * BSS is zero-initialised so every PTE starts as "not present" -- no memset needed.
 */
#define KERNEL_PT_POOL_SIZE 8
static uint32_t kernel_pt_pool[KERNEL_PT_POOL_SIZE][1024] __attribute__((aligned(4096)));
static unsigned kernel_pt_pool_used = 0;

static uint32_t *kpt_alloc(void)
{
    if (kernel_pt_pool_used >= KERNEL_PT_POOL_SIZE) return NULL;
    return kernel_pt_pool[kernel_pt_pool_used++];
}

void paging_initialize(void)
{
    /* boot.S already enabled paging using 4-MiB PSE pages (boot_pd).
     * Switch to a permanent 4-KiB-granularity kernel page directory.
     *
     * first_page_table maps 1024 × 4-KiB pages spanning 0x00000000-0x003FFFFF.
     * We install it in TWO page directory slots so that both the identity
     * region AND the kernel-high region (0xC0000000-0xC03FFFFF) point to the
     * same physical pages.                                                   */
    for (int i = 0; i < 1024; i++)
        first_page_table[i] = paging_make_entry(
            (uint32_t)i * PAGE_SIZE, PAGE_PRESENT | PAGE_WRITABLE);

    /* Physical address of the page table (linked at high VA). */
    uint32_t pt_phys = (uint32_t)(uintptr_t)first_page_table - KERNEL_OFFSET;

    /* PD[0]: identity map 0x00000000-0x003FFFFF. */
    page_directory[0] = paging_make_entry(pt_phys, PAGE_PRESENT | PAGE_WRITABLE);

    /* PD[768]: kernel-high map 0xC0000000-0xC03FFFFF -> same physical pages. */
    page_directory[KERNEL_PD_IDX] = paging_make_entry(
        pt_phys, PAGE_PRESENT | PAGE_WRITABLE);

    /* PD[769]: pre-wired page table for 0xC0400000-0xC07FFFFF.
     * The VBE shadow buffer is mapped here (VBE_SHADOW_VBASE=0xC0500000).
     * Using a static BSS table means paging_map_page_alloc() never needs
     * PMM for this PD slot -- by VBE init time free pages may be above the
     * 4 MiB identity-mapped window and would crash if used as a PT.      */
    uint32_t pt2_phys = (uint32_t)(uintptr_t)second_page_table - KERNEL_OFFSET;
    page_directory[KERNEL_PD_IDX + 1] = paging_make_entry(
        pt2_phys, PAGE_PRESENT | PAGE_WRITABLE);

    /* Switch CR3 to the physical address of the permanent kernel PD. */
    uint32_t pd_phys = (uint32_t)(uintptr_t)page_directory - KERNEL_OFFSET;
    asm volatile(
        "mov %0, %%cr3\n\t"
        "mov %%cr0, %%eax\n\t"
        "or  $0x80000000, %%eax\n\t"
        "mov %%eax, %%cr0\n\t"
        :
        : "r"(pd_phys)
        : "eax"
    );
}

/* -- kmap: temporary kernel access to an arbitrary physical page ------------
 *
 * The kernel can only dereference a physical address directly while it falls in
 * the first 4 MiB identity map.  Data pages (heap, ELF segments, stack, CoW
 * copies) are allocated above 4 MiB (pmm_alloc_page_above_4mib) to keep the
 * scarce identity-mapped low memory for paging structures, so the kernel needs
 * a window to reach them.  kmap() installs a transient mapping of a physical
 * page at a fixed kernel VA, returns a pointer, and kunmap() tears it down.
 *
 * The window lives in second_page_table (PD[769], 0xC0400000-0xC04FFFFF), which
 * is copied into every process address space by paging_create_address_space().
 * That matters because the page-fault handler runs under the FAULTING process's
 * CR3 -- the window must resolve there too, not just under the kernel PD.
 *
 * KMAP_SLOTS independent slots allow a few concurrent/nested maps (e.g. CoW does
 * two at once).  A spinlock guards slot allocation.  Slot i maps to
 * KMAP_BASE + i*PAGE_SIZE.  The window sits below the VBE shadow (0xC0500000),
 * which uses higher offsets of the same page table.
 */
#define KMAP_BASE   0xC0400000u
#define KMAP_SLOTS  8u
/* PTE index within second_page_table for KMAP_BASE: (0xC0400000 >> 12) & 0x3FF. */
#define KMAP_PTE0   (((KMAP_BASE) >> 12) & 0x3FFu)

static spinlock_t kmap_lock = SPINLOCK_INIT;
static uint8_t    kmap_used[KMAP_SLOTS];

void *kmap(uint32_t phys)
{
    /* IRQ-safe: kmap is used by the page-fault handler (IF=0). */
    uint32_t f = spinlock_acquire_irqsave(&kmap_lock);
    int slot = -1;
    for (uint32_t i = 0; i < KMAP_SLOTS; i++) {
        if (!kmap_used[i]) { kmap_used[i] = 1; slot = (int)i; break; }
    }
    spinlock_release_irqrestore(&kmap_lock, f);
    if (slot < 0) return NULL;   /* all slots busy -- caller must serialise */

    uint32_t va = KMAP_BASE + (uint32_t)slot * PAGE_SIZE;
    second_page_table[KMAP_PTE0 + (uint32_t)slot] =
        paging_make_entry(phys & ~(uint32_t)0xFFF, PAGE_PRESENT | PAGE_WRITABLE);
    asm volatile("invlpg (%0)" :: "r"(va) : "memory");
    return (void *)(uintptr_t)(va + (phys & 0xFFFu));
}

void kunmap(void *ptr)
{
    uint32_t va   = (uint32_t)(uintptr_t)ptr & ~(uint32_t)0xFFF;
    if (va < KMAP_BASE || va >= KMAP_BASE + KMAP_SLOTS * PAGE_SIZE) return;
    uint32_t slot = (va - KMAP_BASE) / PAGE_SIZE;

    second_page_table[KMAP_PTE0 + slot] = 0;   /* not present */
    asm volatile("invlpg (%0)" :: "r"(va) : "memory");

    uint32_t f = spinlock_acquire_irqsave(&kmap_lock);
    kmap_used[slot] = 0;
    spinlock_release_irqrestore(&kmap_lock, f);
}

/* Allocate a zeroed data page above the identity map, accessible via kmap.
 * Returns the physical address (NULL on OOM).  Used for heap/ELF/stack/CoW
 * pages -- everything the kernel only needs transient access to. */
void *pmm_alloc_data_page(void)
{
    void *phys = pmm_alloc_page_above_4mib();
    if (!phys) phys = pmm_alloc_page();   /* fall back to low memory if high is full */
    if (!phys) return NULL;
    void *v = kmap((uint32_t)(uintptr_t)phys);
    if (v) { memset(v, 0, PAGE_SIZE); kunmap(v); }
    else {
        /* No kmap slot: if the page happens to be identity-mapped, zero it
         * directly; otherwise leave it (caller will kmap to use it anyway). */
        if ((uint32_t)(uintptr_t)phys < 0x400000u)
            memset(phys, 0, PAGE_SIZE);
    }
    return phys;
}

void paging_set_user_access(uint32_t virt_start, uint32_t virt_end)
{
    /* Operate on the ACTIVE page directory (whatever is currently in CR3),
     * not the kernel's global page_directory[].  This ensures the function
     * works correctly both when called from the kernel context and when
     * called after paging_switch() has loaded a process's private PD.    */
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    uint32_t *active_pd = (uint32_t *)(uintptr_t)cr3;

    /* Walk every page in [virt_start, virt_end) and add PAGE_USER to
     * the PDE and PTE for any page that is already marked present.
     * Silently skips pages that have no mapping yet.                    */
    for (uint32_t addr = virt_start; addr < virt_end; addr += PAGE_SIZE) {
        uint32_t pd_idx = VIRT_PD_INDEX(addr);
        uint32_t pt_idx = VIRT_PT_INDEX(addr);

        if (!(active_pd[pd_idx] & PAGE_PRESENT))
            continue;

        /* Set USER on the page directory entry covering this 4-MiB slot. */
        active_pd[pd_idx] |= PAGE_USER;

        /* Dereference the page table pointer stored in the PDE. */
        uint32_t *pt = (uint32_t *)(active_pd[pd_idx] & ~0xFFFu);
        if (pt[pt_idx] & PAGE_PRESENT)
            pt[pt_idx] |= PAGE_USER;
    }

    /* Flush the entire TLB by reloading CR3. */
    asm volatile(
        "mov %%cr3, %%eax\n\t"
        "mov %%eax, %%cr3\n\t"
        :: : "eax"
    );
}

int paging_map_page(uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t pd_idx = VIRT_PD_INDEX(virt);
    uint32_t pt_idx = VIRT_PT_INDEX(virt);

    if (!(page_directory[pd_idx] & PAGE_PRESENT))
        return -1;   /* no page table for this PD slot */

    uint32_t *pt = (uint32_t *)(page_directory[pd_idx] & ~0xFFFu);
    pt[pt_idx] = paging_make_entry(phys, flags);

    /* Propagate any new flags (e.g. PAGE_USER) up to the PDE too. */
    page_directory[pd_idx] |= (flags & (PAGE_USER | PAGE_WRITABLE));

    /* Invalidate just this one TLB entry. */
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");

    return 0;
}

/* -- Per-process address space helpers (section 5.1) --------------------- */

/*
 * paging_get_kernel_pd -- return a pointer to the kernel page directory.
 *
 * Used when restoring the kernel's address space after running a process
 * in its own page directory (e.g. after exec_longjmp in shell_cmd_exec).
 */
uint32_t *paging_get_kernel_pd(void)
{
    return page_directory;
}

uint32_t paging_kernel_cr3(void)
{
    return (uint32_t)(uintptr_t)page_directory - KERNEL_OFFSET;
}

/*
 * paging_create_address_space -- allocate a new page directory for a process.
 *
 * Allocates a fresh 4-KiB-aligned page directory via pmm_alloc_page() and
 * copies the kernel's PD entry 0 (the first 4-MiB identity-mapped page table)
 * into slot 0 of the new directory.
 *
 * By sharing the same physical page table, every process can reach the kernel
 * at the same virtual addresses as the kernel itself.  User pages (e.g. the
 * ELF segments at 0x400000) are mapped in separate slots, so they are private
 * to each process even if two processes load at the same virtual address.
 *
 * Returns the new page directory pointer (physical == virtual because the
 * PMM allocates from the identity-mapped first 4 MiB), or NULL on OOM.
 */
uint32_t *paging_create_address_space(void)
{
    uint32_t *new_pd = (uint32_t *)pmm_alloc_page();
    if (!new_pd) return NULL;
    memset(new_pd, 0, PAGE_SIZE);

    /* Share the identity page table (PD[0]) so ring-0 code remains
     * reachable immediately after a CR3 switch.
     * Copy ALL upper-half kernel entries (PD[768..1023]) so that any
     * kernel mapping added before this call (including the VBE framebuffer,
     * MMIO regions, etc.) is visible in the new address space.            */
    new_pd[0] = page_directory[0];
    for (int _i = KERNEL_PD_IDX; _i < 1024; _i++)
        new_pd[_i] = page_directory[_i];

    return new_pd;
}

/*
 * paging_switch -- switch the active page directory to pd_phys.
 *
 * Writing CR3 atomically replaces the MMU's view of virtual memory and
 * flushes all TLB entries (except global pages, which we don't use).
 * After this instruction, every memory access uses the new mapping.
 */
void paging_switch(uint32_t pd_phys)
{
    asm volatile("mov %0, %%cr3" :: "r"(pd_phys) : "memory");
}

/*
 * paging_map_page_alloc_into -- map a page into an arbitrary page directory.
 *
 * Like paging_map_page_alloc() but operates on pd[] instead of the global
 * page_directory[].  Used by elf_load_into() to populate the page directory
 * of a child process before it is first scheduled.
 *
 * Because all PMM pages are allocated from the identity-mapped first 4 MiB,
 * we can access the target page directory and any page tables it points to
 * using their physical addresses directly (phys addr == virt addr there).
 *
 * No TLB flush is performed: the target page directory is not currently
 * loaded in CR3, so there are no stale TLB entries to invalidate.
 */
int paging_fork_address_space(uint32_t *parent_pd, uint32_t *child_pd)
{
    /* Entry 0 is the shared kernel identity page table -- already copied into
     * child_pd[0] by paging_create_address_space().  ALL kernel-high entries
     * (KERNEL_PD_IDX..1023) are likewise shared, not per-process: PD[768] is
     * the kernel-high map and PD[769] is the kmap/VBE window whose page table
     * (second_page_table) the kernel mutates live.  Deep-copying any of them
     * would give the child a stale snapshot -- e.g. it would never see kmap()'s
     * PTE writes, faulting not-present on a valid kmap VA.  So fork only the
     * user range [1, KERNEL_PD_IDX).                                          */
    for (int i = 1; i < (int)KERNEL_PD_IDX; i++) {
        if (!(parent_pd[i] & PAGE_PRESENT))
            continue;

        uint32_t *parent_pt = (uint32_t *)(parent_pd[i] & ~(uint32_t)0xFFF);

        uint32_t *child_pt = (uint32_t *)pmm_alloc_page();
        if (!child_pt) return -1;
        memset(child_pt, 0, PAGE_SIZE);

        /* Install in child PD.  Preserve parent PDE flags except WRITABLE:
         * a PDE containing only CoW pages should not advertise writability
         * at the directory level, as that is still per-PTE.               */
        child_pd[i] = paging_make_entry(
            (uint32_t)(uintptr_t)child_pt,
            parent_pd[i] & (uint32_t)0xFFF);

        for (int j = 0; j < 1024; j++) {
            if (!(parent_pt[j] & PAGE_PRESENT))
                continue;

            uint32_t phys  = parent_pt[j] & ~(uint32_t)0xFFF;
            uint32_t flags = parent_pt[j] & (uint32_t)0xFFF;

            /* Both processes now reference this physical page. */
            pmm_ref_page((void *)(uintptr_t)phys);

            if (flags & PAGE_WRITABLE) {
                /* Mark as CoW: clear WRITABLE, set PAGE_COW in both. */
                uint32_t cow_flags = (flags & ~(uint32_t)PAGE_WRITABLE) | PAGE_COW;
                parent_pt[j] = paging_make_entry(phys, cow_flags);
                child_pt[j]  = paging_make_entry(phys, cow_flags);
            } else {
                /* Read-only: share the page unchanged. */
                child_pt[j] = paging_make_entry(phys, flags);
            }
        }
    }
    return 0;
}

int paging_cow_handle(uint32_t *pd, uint32_t fault_addr)
{
    uint32_t pd_idx = VIRT_PD_INDEX(fault_addr);
    uint32_t pt_idx = VIRT_PT_INDEX(fault_addr);

    if (!(pd[pd_idx] & PAGE_PRESENT)) return -1;

    uint32_t *pt  = (uint32_t *)(pd[pd_idx] & ~(uint32_t)0xFFF);
    uint32_t  pte = pt[pt_idx];

    if (!(pte & PAGE_COW)) return -1;   /* not a CoW page */

    uint32_t phys  = pte & ~(uint32_t)0xFFF;
    uint32_t flags = pte & (uint32_t)0xFFF;

    if (pmm_page_refcount((void *)(uintptr_t)phys) <= 1) {
        /* Sole owner: just restore writability. */
        uint32_t new_flags = (flags & ~(uint32_t)PAGE_COW) | PAGE_WRITABLE;
        pt[pt_idx] = paging_make_entry(phys, new_flags);
    } else {
        /* Shared: copy the page, drop our reference to the original.
         * Both the source (an existing data page) and the destination may live
         * above the identity map, so reach each through a transient kmap rather
         * than dereferencing the physical address directly. */
        void *new_phys = pmm_alloc_page_above_4mib();
        if (!new_phys) new_phys = pmm_alloc_page();
        if (!new_phys) return -1;

        void *dst = kmap((uint32_t)(uintptr_t)new_phys);
        void *src = kmap(phys);
        if (!dst || !src) {
            if (dst) kunmap(dst);
            if (src) kunmap(src);
            pmm_free_page(new_phys);
            return -1;
        }
        memcpy(dst, src, PAGE_SIZE);
        kunmap(src);
        kunmap(dst);

        pmm_free_page((void *)(uintptr_t)phys);  /* decrement shared refcount */
        uint32_t new_flags = (flags & ~(uint32_t)PAGE_COW) | PAGE_WRITABLE;
        pt[pt_idx] = paging_make_entry((uint32_t)(uintptr_t)new_phys, new_flags);
    }

    asm volatile("invlpg (%0)" :: "r"(fault_addr) : "memory");
    return 0;
}

int paging_map_page_alloc_into(uint32_t *pd, uint32_t virt,
                                uint32_t phys, uint32_t flags)
{
    uint32_t pd_idx = VIRT_PD_INDEX(virt);
    uint32_t pt_idx = VIRT_PT_INDEX(virt);

    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        void *new_pt = pmm_alloc_page();
        if (!new_pt) return -1;
        memset(new_pt, 0, PAGE_SIZE);
        pd[pd_idx] = paging_make_entry(
            (uint32_t)(uintptr_t)new_pt, PAGE_PRESENT | PAGE_WRITABLE);
    }

    uint32_t *pt = (uint32_t *)(pd[pd_idx] & ~(uint32_t)0xFFF);
    pt[pt_idx] = paging_make_entry(phys, flags);
    pd[pd_idx] |= (flags & (PAGE_USER | PAGE_WRITABLE));

    return 0;
}

void paging_map_mmio(uint32_t virt, uint32_t phys)
{
    paging_map_page_alloc(virt, phys,
                          PAGE_PRESENT | PAGE_WRITABLE | PAGE_NOCACHE);
}

int paging_map_page_alloc(uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t pd_idx = VIRT_PD_INDEX(virt);
    uint32_t pt_idx = VIRT_PT_INDEX(virt);

    if (!(page_directory[pd_idx] & PAGE_PRESENT)) {
        /* No page table exists for this 4-MiB slot -- take one from the
         * static BSS pool.  The pool lives within the first 4 MiB
         * (physical = VA - KERNEL_OFFSET) so its physical address is
         * always within the identity-mapped region and can be stored
         * directly in the PDE.  BSS is zero-initialised, so every PTE
         * starts as "not present" with no memset required.             */
        uint32_t *new_pt = kpt_alloc();
        if (!new_pt) return -1;

        page_directory[pd_idx] = paging_make_entry(
            (uint32_t)(uintptr_t)new_pt - KERNEL_OFFSET,
            PAGE_PRESENT | PAGE_WRITABLE);
    }

    /* The PDE stores the PHYSICAL address of the PT.  Within the first
     * 4 MiB the identity map makes phys == virt, so casting to a pointer
     * and dereferencing works for all pool-backed PTs.                  */
    uint32_t *pt = (uint32_t *)(page_directory[pd_idx] & ~(uint32_t)0xFFF);
    pt[pt_idx] = paging_make_entry(phys, flags);

    /* Propagate PAGE_USER / PAGE_WRITABLE to the PDE so the CPU allows
     * ring-3 accesses through the page directory entry too.             */
    page_directory[pd_idx] |= (flags & (PAGE_USER | PAGE_WRITABLE));

    /* Invalidate just this one TLB entry. */
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");

    return 0;
}
