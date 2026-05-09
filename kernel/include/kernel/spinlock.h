#ifndef _KERNEL_SPINLOCK_H
#define _KERNEL_SPINLOCK_H

#include <stdint.h>

/* A spinlock is a single word; 0 = unlocked, 1 = locked. */
typedef struct { volatile uint32_t locked; } spinlock_t;

#define SPINLOCK_INIT { 0 }

/* Try to acquire the lock once.  Returns 1 on success, 0 if already held.
 * Pure C (GCC builtin atomic) — usable from both the kernel and host tests. */
static inline int spinlock_trylock(spinlock_t *lk)
{
    return __sync_bool_compare_and_swap(&lk->locked, 0u, 1u);
}

/* Release the lock.  Must only be called by the thread that holds it. */
static inline void spinlock_release(spinlock_t *lk)
{
    __sync_bool_compare_and_swap(&lk->locked, 1u, 0u);
}

/* spinlock_is_locked — return 1 if lock appears held, 0 otherwise.
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
#else
/* Host-side stub: single attempt (tests run single-threaded). */
static inline void spinlock_acquire(spinlock_t *lk)
{
    spinlock_trylock(lk);
}
#endif /* __is_kernel */

#endif /* _KERNEL_SPINLOCK_H */
