/*
 * Quilon user-space libc -- stdlib.c
 *
 * malloc/free -- first-fit block allocator on top of sbrk().
 * atoi        -- string to integer conversion.
 * exit        -- declared here for stdlib.h but implemented in syscall.S.
 *
 * Heap layout
 * -----------
 * The heap grows upward from USER_HEAP_START (0x800000, set by SYS_SBRK
 * when heap_end == 0 in the process's PCB).  Each allocation is preceded
 * by a block_t header:
 *
 *   [ block_t header (12 bytes) ][ user data (size bytes) ]
 *
 * The linked list runs through all headers.  malloc() does a first-fit
 * search; free() marks a block free.  Adjacent free blocks are coalesced
 * on the next malloc pass.
 *
 * Thread safety: malloc/free are guarded by a spinlock.  CLONE_VM threads
 * (e.g. the window manager and its apps) share one heap and are preemptively
 * scheduled -- and may run on different CPUs under SMP -- so unsynchronised
 * access would corrupt the free list (a classic symptom is a hang in the
 * coalescing walk).  The lock serialises every heap mutation, including the
 * sbrk() that extends it.
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>

/* -- Block header ------------------------------------------------------- */

typedef struct block {
    unsigned int   size;    /* usable bytes (not including the header)    */
    int            free;    /* 1 = available, 0 = in use                  */
    struct block  *next;    /* next block in the list, or NULL            */
} block_t;

#define BLOCK_HDR_SZ  sizeof(block_t)   /* 12 bytes on i386 */

static block_t *heap_head = (block_t *)0;  /* NULL -- not yet initialised  */

/* -- Heap lock ----------------------------------------------------------- *
 * A minimal test-and-set spinlock.  xchg is atomic on x86 (it asserts LOCK
 * implicitly), so this is correct across both preemption and multiple CPUs.
 * The critical sections are short (free-list walks), so spinning is fine. */
static volatile int heap_lock = 0;

static void heap_lock_acquire(void)
{
    while (__sync_lock_test_and_set(&heap_lock, 1))
        /* spin until the previous holder releases */;
}

static void heap_lock_release(void)
{
    __sync_lock_release(&heap_lock);
}

/* -- Helpers ------------------------------------------------------------ */

/* Align size up to the next 4-byte boundary. */
static unsigned int align4(unsigned int n)
{
    return (n + 3u) & ~3u;
}

/* Extend the heap by (BLOCK_HDR_SZ + size) bytes and return a new block. */
static block_t *heap_extend(unsigned int size)
{
    block_t *b = (block_t *)sbrk((int)(BLOCK_HDR_SZ + size));
    if ((int)(unsigned int)(unsigned long)b == -1)
        return (block_t *)0;   /* OOM */
    b->size = size;
    b->free = 0;
    b->next = (block_t *)0;
    return b;
}

/* -- Public API --------------------------------------------------------- */

/* Heap allocation core -- caller must hold heap_lock. */
static void *malloc_locked(unsigned int size)
{
    /* First call: initialise the heap. */
    if (!heap_head) {
        heap_head = heap_extend(size);
        if (!heap_head) return (void *)0;
        return (void *)(heap_head + 1);
    }

    /* Search for a suitable free block (first-fit with coalescing). */
    block_t *b    = heap_head;
    block_t *prev = (block_t *)0;

    while (b) {
        /* Coalesce adjacent free blocks before checking fit. */
        while (b->free && b->next && b->next->free) {
            b->size += BLOCK_HDR_SZ + b->next->size;
            b->next  = b->next->next;
        }
        if (b->free && b->size >= size) {
            /* Split the block if there is enough room for another header
             * plus at least 4 bytes of usable space. */
            if (b->size >= size + BLOCK_HDR_SZ + 4u) {
                block_t *split = (block_t *)((char *)(b + 1) + size);
                split->size    = b->size - size - BLOCK_HDR_SZ;
                split->free    = 1;
                split->next    = b->next;
                b->next        = split;
                b->size        = size;
            }
            b->free = 0;
            return (void *)(b + 1);
        }
        prev = b;
        b    = b->next;
    }

    /* No free block found: extend the heap. */
    block_t *new_block = heap_extend(size);
    if (!new_block) return (void *)0;
    if (prev) prev->next = new_block;
    return (void *)(new_block + 1);
}

void *malloc(size_t sz)
{
    if (sz == 0) return (void *)0;

    unsigned int size = align4((unsigned int)sz);

    heap_lock_acquire();
    void *p = malloc_locked(size);
    heap_lock_release();
    return p;
}

void free(void *ptr)
{
    if (!ptr) return;
    /* The store to b->free races with malloc's free-list walk if unlocked. */
    heap_lock_acquire();
    block_t *b = (block_t *)ptr - 1;
    b->free = 1;
    /* Coalescing deferred to the next malloc() pass. */
    heap_lock_release();
}

int atoi(const char *s)
{
    int n    = 0;
    int sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if      (*s == '-') { sign = -1; s++; }
    else if (*s == '+') {             s++; }
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return sign * n;
}
