#include <stdint.h>
#include <string.h>

#include <kernel/pmm.h>
#include <kernel/multiboot.h>
#include <kernel/paging.h>
#include <kernel/spinlock.h>

/* ── Bitmap ─────────────────────────────────────────────────────────────── */
/* One bit per 4 KiB page across the full 32-bit (4 GiB) address space.
 * 4 GiB / 4 KiB = 1 048 576 pages → bitmap is 128 KiB, stored in .bss.
 * Bit = 1 → page is used/reserved.  Bit = 0 → page is free.              */

#define MAX_PAGES   (0x100000u)           /* 4 GiB / 4 KiB               */
#define BITMAP_WORDS (MAX_PAGES / 32u)    /* 32 pages per uint32_t word  */

static uint32_t bitmap[BITMAP_WORDS];
static uint32_t free_pages = 0;

/* Protects bitmap and free_pages against concurrent access from multiple
 * CPUs (SMP section 10.5).  Initialised statically so it is ready before
 * any kernel code runs.                                                    */
static spinlock_t pmm_lock = SPINLOCK_INIT;

/* ── Reference counts (section 9.3 — CoW) ──────────────────────────────── */
/* One byte per physical page.  Starts at 1 on alloc; pmm_ref_page
 * increments it; pmm_free_page decrements it and only releases the
 * page when the count reaches 0.  max refcount per page: 255. */
static uint8_t refcount[MAX_PAGES];

/* Linker-defined symbols — use their *addresses*, not their contents. */
extern uint32_t kernel_start;
extern uint32_t kernel_end;

/* ── Bitmap primitives ───────────────────────────────────────────────────── */

static inline void page_set(uint32_t page)
{
    bitmap[page / 32] |= (1u << (page % 32));
}

static inline void page_clear(uint32_t page)
{
    bitmap[page / 32] &= ~(1u << (page % 32));
}

static inline int page_test(uint32_t page)
{
    return (bitmap[page / 32] >> (page % 32)) & 1u;
}

/* ── Range helpers ───────────────────────────────────────────────────────── */

static void pmm_free_range(uint32_t base, uint32_t length)
{
    uint32_t first = base / PAGE_SIZE;
    uint32_t last  = (base + length) / PAGE_SIZE;   /* exclusive */
    for (uint32_t p = first; p < last && p < MAX_PAGES; p++) {
        if (page_test(p)) {
            page_clear(p);
            free_pages++;
        }
    }
}

static void pmm_reserve_range(uint32_t base, uint32_t length)
{
    /* Round base down, round end up — never leave a partial page free. */
    uint32_t first = base / PAGE_SIZE;
    uint32_t last  = (base + length + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t p = first; p < last && p < MAX_PAGES; p++) {
        if (!page_test(p)) {
            page_set(p);
            free_pages--;
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void pmm_initialize(void *multiboot_info)
{
    multiboot_info_t *mbi = (multiboot_info_t *)multiboot_info;

    /* Start with every page marked used; we'll free what GRUB says is safe. */
    memset(bitmap, 0xFF, sizeof(bitmap));
    free_pages = 0;

    if (mbi->flags & MULTIBOOT_FLAG_MMAP) {
        /* Walk the variable-length memory map provided by GRUB. */
        uintptr_t pos = (uintptr_t)mbi->mmap_addr;
        uintptr_t end = pos + mbi->mmap_length;

        while (pos < end) {
            multiboot_mmap_entry_t *e = (multiboot_mmap_entry_t *)pos;

            if (e->type == MULTIBOOT_MEMORY_AVAILABLE && e->addr < 0x100000000ULL) {
                uint32_t base = (uint32_t)e->addr;
                uint64_t len  = e->len;
                /* Clamp to the 32-bit address space boundary. */
                if (e->addr + len > 0x100000000ULL)
                    len = 0x100000000ULL - e->addr;
                pmm_free_range(base, (uint32_t)len);
            }

            pos += e->size + sizeof(e->size);   /* advance past this entry */
        }
    } else if (mbi->flags & MULTIBOOT_FLAG_MEM) {
        /* Fallback: GRUB at least told us how much upper memory there is.
         * Upper memory starts at 1 MiB.                                   */
        pmm_free_range(0x100000, mbi->mem_upper * 1024);
    }

    /* Page 0 (the null page) must never be allocated — keeps NULL special. */
    pmm_reserve_range(0, PAGE_SIZE);

    /* Reserve pages occupied by the kernel image (text + rodata + data + bss,
     * which includes this bitmap array itself).
     * kernel_start/end are linked at high VA; subtract KERNEL_OFFSET for physical. */
    pmm_reserve_range((uint32_t)&kernel_start - KERNEL_OFFSET,
                      (uint32_t)&kernel_end - (uint32_t)&kernel_start);
}

void *pmm_alloc_page(void)
{
    spinlock_acquire(&pmm_lock);
    void *result = NULL;
    for (uint32_t i = 0; i < BITMAP_WORDS; i++) {
        if (bitmap[i] == 0xFFFFFFFFu)
            continue;   /* all 32 pages in this word are used */

        for (int bit = 0; bit < 32; bit++) {
            uint32_t page = i * 32 + bit;
            if (!page_test(page)) {
                page_set(page);
                free_pages--;
                refcount[page] = 1;
                result = (void *)(uintptr_t)(page * PAGE_SIZE);
                goto done;
            }
        }
    }
done:
    spinlock_release(&pmm_lock);
    return result;   /* NULL = out of memory */
}

void pmm_free_page(void *addr)
{
    spinlock_acquire(&pmm_lock);
    uint32_t page = (uint32_t)(uintptr_t)addr / PAGE_SIZE;
    if (!page_test(page)) goto done;   /* page not allocated — no-op */
    if (refcount[page] > 1) {
        refcount[page]--;
        goto done;                     /* still referenced */
    }
    /* Last reference: release the page back to the pool. */
    refcount[page] = 0;
    page_clear(page);
    free_pages++;
done:
    spinlock_release(&pmm_lock);
}

void pmm_ref_page(void *addr)
{
    uint32_t page = (uint32_t)(uintptr_t)addr / PAGE_SIZE;
    if (page_test(page) && refcount[page] < 255u)
        refcount[page]++;
}

uint8_t pmm_page_refcount(void *addr)
{
    uint32_t page = (uint32_t)(uintptr_t)addr / PAGE_SIZE;
    return refcount[page];
}

uint32_t pmm_free_page_count(void)
{
    return free_pages;
}

void pmm_init_range(uint32_t free_base, uint32_t free_len,
                    uint32_t reserved_base, uint32_t reserved_len)
{
    memset(bitmap, 0xFF, sizeof(bitmap));
    memset(refcount, 0, sizeof(refcount));
    free_pages = 0;

    pmm_free_range(free_base, free_len);
    pmm_reserve_range(0, PAGE_SIZE);           /* null page always reserved */
    if (reserved_len > 0)
        pmm_reserve_range(reserved_base, reserved_len);
}
