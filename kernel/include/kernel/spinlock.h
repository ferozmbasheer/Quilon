#ifndef _KERNEL_SPINLOCK_H
#define _KERNEL_SPINLOCK_H

#include <stdint.h>

/* A spinlock is a single word; 0 = unlocked, 1 = locked. */
typedef struct { volatile uint32_t locked; } spinlock_t;

#define SPINLOCK_INIT { 0 }

/* Try to acquire the lock once.  Returns 1 on success, 0 if already held.
 * Pure C (GCC builtin atomic) -- usable from both the kernel and host tests. */
static inline int spinlock_trylock(spinlock_t *lk)
{
    return __sync_bool_compare_and_swap(&lk->locked, 0u, 1u);
}

/* Release the lock.  Must only be called by the thread that holds it. */
static inline void spinlock_release(spinlock_t *lk)
{
    __sync_bool_compare_and_swap(&lk->locked, 1u, 0u);
}

/* spinlock_is_locked -- return 1 if lock appears held, 0 otherwise.
 * Non-atomic peek, useful only for assertions and debug prints. */
static inline int spinlock_is_locked(const spinlock_t *lk)
{
    return (int)(lk->locked != 0u);
}

#ifdef __is_kernel
/* Kernel-only: spin with PAUSE until the lock is acquired.
 * PAUSE hints to the CPU that this is a spin-wait loop, improving
 * performance on Hyper-Threading CPUs and reducing memory traffic. */
static inline void spinlock_acquire(spinlock_t *lk)
{
    while (!__sync_bool_compare_and_swap(&lk->locked, 0u, 1u))
        asm volatile("pause");
}

/* -- IRQ-safe spinlock --------------------------------------------------------
 *
 * A lock that is taken in BOTH thread context and interrupt/exception context
 * MUST disable interrupts while held.  Otherwise: a thread acquires the lock,
 * the timer (or a page fault) preempts it on the same CPU, the handler tries to
 * acquire the same lock, and -- because the handler runs through an interrupt
 * gate (IF=0) and can never yield back to the preempted holder -- it spins
 * forever.  The whole CPU wedges with no panic.
 *
 * spinlock_acquire_irqsave() saves EFLAGS, clears IF, then spins; the returned
 * value is passed back to spinlock_release_irqrestore() to restore the prior
 * interrupt state (so nesting is safe -- an inner release does not prematurely
 * re-enable interrupts).
 *
 * Use these for pmm_lock, kmap_lock, tty_lock and any other lock reachable from
 * an IRQ or exception handler.  Plain spinlock_acquire() remains fine for locks
 * only ever taken in thread context.
 */
static inline uint32_t spinlock_acquire_irqsave(spinlock_t *lk)
{
    uint32_t flags;
    asm volatile("pushf\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    while (!__sync_bool_compare_and_swap(&lk->locked, 0u, 1u))
        asm volatile("pause");
    return flags;
}

static inline void spinlock_release_irqrestore(spinlock_t *lk, uint32_t flags)
{
    __sync_bool_compare_and_swap(&lk->locked, 1u, 0u);
    asm volatile("push %0\n\tpopf" :: "r"(flags) : "memory", "cc");
}
#else
/* Host-side stub: single attempt (tests run single-threaded). */
static inline void spinlock_acquire(spinlock_t *lk)
{
    spinlock_trylock(lk);
}
static inline uint32_t spinlock_acquire_irqsave(spinlock_t *lk)
{
    spinlock_trylock(lk);
    return 0;
}
static inline void spinlock_release_irqrestore(spinlock_t *lk, uint32_t flags)
{
    (void)flags;
    spinlock_release(lk);
}
#endif /* __is_kernel */

#endif /* _KERNEL_SPINLOCK_H */
