/*
 * Quilon OS -- Virtual Memory Area manager (section 9.2)
 *
 * Pure C -- no architecture-specific code.  Compiles and is unit-testable on
 * the host without the cross-compiler.
 */

#include <stdint.h>
#include <stddef.h>
#include <kernel/vma.h>

void vma_init(vma_t *vmas, int count)
{
    for (int i = 0; i < count; i++) {
        vmas[i].start = 0;
        vmas[i].end   = 0;
        vmas[i].flags = 0;
        vmas[i].used  = 0;
    }
}

int vma_add(vma_t *vmas, int count, uint32_t start, uint32_t end, uint32_t flags)
{
    for (int i = 0; i < count; i++) {
        if (!vmas[i].used) {
            vmas[i].start = start;
            vmas[i].end   = end;
            vmas[i].flags = flags;
            vmas[i].used  = 1;
            return 0;
        }
    }
    return -1;   /* table full */
}

vma_t *vma_find(vma_t *vmas, int count, uint32_t addr)
{
    for (int i = 0; i < count; i++) {
        if (vmas[i].used && vmas[i].start <= addr && addr < vmas[i].end)
            return &vmas[i];
    }
    return NULL;
}

vma_t *vma_find_start(vma_t *vmas, int count, uint32_t start)
{
    for (int i = 0; i < count; i++) {
        if (vmas[i].used && vmas[i].start == start)
            return &vmas[i];
    }
    return NULL;
}

int vma_extend(vma_t *vmas, int count, uint32_t start, uint32_t new_end)
{
    vma_t *v = vma_find_start(vmas, count, start);
    if (!v) return -1;
    v->end = new_end;
    return 0;
}

void vma_remove(vma_t *vmas, int count, uint32_t start)
{
    for (int i = 0; i < count; i++) {
        if (vmas[i].used && vmas[i].start == start) {
            vmas[i].used = 0;
            return;
        }
    }
}

/* Page size used for coverage checks.  VMA bounds are always page-aligned, so
 * a page is fully covered by a VMA iff its base address falls inside it. */
#define VMA_PAGE_SIZE 0x1000u

int vma_range_ok(const vma_t *vmas, int count,
                 uint32_t addr, uint32_t len, int need_write)
{
    if (len == 0)
        return 1;                       /* nothing to access */

    /* Reject ranges that wrap around the 32-bit address space. */
    if (addr + len < addr)
        return 0;

    uint32_t need  = need_write ? VMA_W : VMA_R;
    uint32_t last  = addr + len - 1;
    uint32_t first = addr & ~(VMA_PAGE_SIZE - 1u);
    uint32_t lastp = last & ~(VMA_PAGE_SIZE - 1u);

    /* Number of pages the range touches.  Counting iterations (rather than
     * comparing page <= lastp) avoids an infinite loop when the range reaches
     * the top page and page += PAGE_SIZE would wrap to 0. */
    uint32_t npages = (lastp - first) / VMA_PAGE_SIZE + 1u;
    uint32_t page   = first;

    for (uint32_t n = 0; n < npages; n++, page += VMA_PAGE_SIZE) {
        int covered = 0;
        for (int i = 0; i < count; i++) {
            if (vmas[i].used &&
                vmas[i].start <= page && page < vmas[i].end &&
                (vmas[i].flags & need)) {
                covered = 1;
                break;
            }
        }
        if (!covered)
            return 0;
    }
    return 1;
}
