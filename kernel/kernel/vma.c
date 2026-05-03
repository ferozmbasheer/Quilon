/*
 * Quilon OS — Virtual Memory Area manager (section 9.2)
 *
 * Pure C — no architecture-specific code.  Compiles and is unit-testable on
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
