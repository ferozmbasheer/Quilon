#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <kernel/kmalloc.h>
#include <kernel/paging.h>   /* PAGE_SIZE */

/* -- Block header ------------------------------------------------------------
 *
 * Layout in memory (addresses increase ->):
 *
 *   [ block_header_t | ← size bytes of user data -> | next block_header_t | …]
 *
 * `size` is the number of *usable* bytes that follow this header (not counting
 * the header itself).  The next header (if any) starts at:
 *
 *   (uint8_t *)header + sizeof(block_header_t) + header->size
 */

typedef struct block_header {
    size_t            size;    /* usable bytes after the header               */
    uint32_t          magic;   /* HEAP_MAGIC -- detects corruption / bad ptrs  */
    int               is_free; /* 1 = free, 0 = allocated                     */
    struct block_header *next; /* next block, or NULL if this is the last one */
} block_header_t;

#define HEADER_SZ   sizeof(block_header_t)

/* Minimum user-data size left over in the *remainder* block after a split.
 * Splitting a block produces a new header; only worth doing if the tail has
 * at least this many usable bytes, otherwise the new header wastes more space
 * than it saves.                                                               */
#define MIN_SPLIT   (HEADER_SZ + 8u)

/* All allocations are rounded up to a multiple of ALIGN bytes so that the
 * returned pointer is always suitably aligned for any basic type.             */
#define ALIGN       8u

/* Round `n` up to the next multiple of ALIGN. */
static inline size_t align_up(size_t n)
{
    return (n + ALIGN - 1u) & ~(ALIGN - 1u);
}

/* -- Heap state --------------------------------------------------------------
 *
 * The heap is a contiguous byte range starting at `heap_base` and covering
 * `heap_bytes` bytes.  The linker-defined symbol `kernel_end` gives us the
 * first free byte after the kernel image; we place the heap immediately after.
 */

extern uint32_t kernel_end;      /* defined in linker.ld */

static block_header_t *heap_head = NULL;   /* first block in the linked list */

/* -- kmalloc_initialize ---------------------------------------------------- */

void kmalloc_initialize(void)
{
    /* Align the heap start to ALIGN bytes.  `kernel_end` is the first
     * address past the kernel's bss; it may not be aligned.               */
    uintptr_t start = ((uintptr_t)&kernel_end + ALIGN - 1u) & ~(ALIGN - 1u);
    uintptr_t end   = start + HEAP_SIZE;

    /* Sanity check: the kernel is mapped into a 4 MiB window at KERNEL_OFFSET
     * (0xC0000000-0xC03FFFFF).  Verify the heap fits within that window.   */
    if (end > KERNEL_OFFSET + 4u * 1024u * 1024u) {
        printf("kmalloc: PANIC -- heap [0x%x, 0x%x) exceeds kernel 4 MiB window\r\n",
               (unsigned)start, (unsigned)end);
        for (;;) asm volatile("hlt");
    }

    /* Initialise a single free block that spans the whole heap. */
    heap_head          = (block_header_t *)start;
    heap_head->size    = HEAP_SIZE - HEADER_SZ;
    heap_head->magic   = HEAP_MAGIC;
    heap_head->is_free = 1;
    heap_head->next    = NULL;
}

/* -- kmalloc --------------------------------------------------------------- */

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    size_t need = align_up(size);

    /* First-fit search. */
    for (block_header_t *b = heap_head; b != NULL; b = b->next) {
        if (!b->is_free || b->size < need)
            continue;

        /* Optionally split the block if the leftover would be large enough
         * to hold at least one useful allocation later.                    */
        if (b->size >= need + MIN_SPLIT) {
            /* Carve out a new free block immediately after the user data. */
            block_header_t *tail =
                (block_header_t *)((uint8_t *)b + HEADER_SZ + need);
            tail->size    = b->size - need - HEADER_SZ;
            tail->magic   = HEAP_MAGIC;
            tail->is_free = 1;
            tail->next    = b->next;

            b->size = need;
            b->next = tail;
        }

        b->is_free = 0;
        /* Return the address immediately after the header. */
        return (void *)((uint8_t *)b + HEADER_SZ);
    }

    /* No suitable block found. */
    printf("kmalloc: out of heap memory (requested %d bytes)\r\n",
           (int)size);
    return NULL;
}

/* -- kfree ----------------------------------------------------------------- */

void kfree(void *ptr)
{
    if (ptr == NULL)
        return;

    block_header_t *b = (block_header_t *)((uint8_t *)ptr - HEADER_SZ);

    /* Validate the magic canary before touching anything. */
    if (b->magic != HEAP_MAGIC) {
        printf("kfree: WARNING -- bad magic at 0x%x (got 0x%x), ignoring\r\n",
               (unsigned)(uintptr_t)ptr, (unsigned)b->magic);
        return;
    }
    if (b->is_free) {
        /* Already free -- double-free guard. */
        printf("kfree: WARNING -- double-free detected at 0x%x, ignoring\r\n",
               (unsigned)(uintptr_t)ptr);
        return;
    }

    b->is_free = 1;

    /* Coalesce: walk the list from the head and merge any run of adjacent
     * free blocks into one.  A single forward pass is sufficient because
     * kmalloc always splits from the left and the list is in address order. */
    for (block_header_t *cur = heap_head; cur != NULL; cur = cur->next) {
        while (cur->is_free && cur->next != NULL && cur->next->is_free) {
            block_header_t *nxt = cur->next;
            /* Absorb nxt into cur. */
            cur->size += HEADER_SZ + nxt->size;
            cur->next  = nxt->next;
            /* Poison the absorbed header to catch stale pointers. */
            nxt->magic = 0xDEAD0000u;
        }
    }
}

/* -- kmalloc_dump ---------------------------------------------------------- */

void kmalloc_dump(void)
{
    printf("kmalloc heap dump:\r\n");
    int n = 0;
    size_t free_bytes = 0, used_bytes = 0;
    for (block_header_t *b = heap_head; b != NULL; b = b->next) {
        printf("  [%d] addr=0x%x size=%d %s\r\n",
               n,
               (unsigned)((uintptr_t)b + HEADER_SZ),
               (int)b->size,
               b->is_free ? "FREE" : "USED");
        if (b->is_free) free_bytes += b->size;
        else            used_bytes += b->size;
        n++;
    }
    printf("  total blocks=%d  free=%d B  used=%d B\r\n",
           n, (int)free_bytes, (int)used_bytes);
}
