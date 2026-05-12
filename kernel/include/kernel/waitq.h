/*
 * Quilon OS — Wait Queues (section 12.2)
 *
 * A wait queue is a singly-linked list of blocked processes.  Instead of
 * spin-waiting ("poll until data"), a consumer pushes itself onto the queue
 * and calls scheduler_yield().  When data arrives (in an IRQ handler or a
 * producer), waitq_wake_one / waitq_wake_all marks sleepers PROC_READY so
 * the scheduler can resume them.
 *
 * Design notes
 * ────────────
 * • waitq_entry_t is stack-allocated inside waitq_sleep.  The entry is
 *   self-removed before waitq_sleep returns, so there are no dangling
 *   pointers after the calling frame unwinds.
 *
 * • waitq_sleep is a no-op when current_process is NULL (early boot,
 *   before the scheduler is running).  Callers that wrap it in a while
 *   loop degrade gracefully to a busy-wait in that case.
 *
 * • waitq_wake_one / waitq_wake_all may be called from IRQ context
 *   because they only set process states and advance list pointers —
 *   no allocation, no locks needed.
 *
 * Typical usage
 * ─────────────
 *   Producer (IRQ handler or writer):
 *     data_ready = 1;
 *     waitq_wake_all(&wq);
 *
 *   Consumer (blocking read):
 *     while (!data_ready)
 *         waitq_sleep(&wq);
 */

#ifndef _KERNEL_WAITQ_H
#define _KERNEL_WAITQ_H

#include <stddef.h>   /* NULL */

/* Forward declaration — avoids pulling process.h into every header that
 * includes waitq.h (e.g. pipe.h).  waitq.c resolves the full definition. */
struct process;

typedef struct waitq_entry {
    struct process     *proc;   /* the sleeping process                */
    struct waitq_entry *next;   /* next entry in the list (LIFO order) */
} waitq_entry_t;

typedef struct {
    waitq_entry_t *head;   /* head of the sleeper list; NULL when empty */
} waitq_t;

/* Static initialiser: waitq_t wq = WAITQ_INIT; */
#define WAITQ_INIT { NULL }

/*
 * waitq_sleep — block the current process on this queue.
 *
 * Pushes a stack-allocated entry onto wq->head, sets current_process to
 * PROC_BLOCKED, calls scheduler_yield(), then on return removes the entry
 * from the list and restores the state to PROC_RUNNING.
 *
 * If current_process is NULL (no scheduler yet) the function returns
 * immediately without touching the queue — the outer loop spins instead.
 *
 * Must be called inside a predicate loop to handle spurious wakeups:
 *   while (!condition) waitq_sleep(&wq);
 */
void waitq_sleep(waitq_t *wq);

/*
 * waitq_wake_one — wake one process waiting on this queue.
 *
 * Removes the head entry and sets the process to PROC_READY.
 * No-op if the queue is empty.  Safe to call from an IRQ handler.
 */
void waitq_wake_one(waitq_t *wq);

/*
 * waitq_wake_all — wake every process waiting on this queue.
 *
 * Atomically detaches the entire list and sets every sleeper to PROC_READY.
 * No-op if the queue is empty.  Safe to call from an IRQ handler.
 */
void waitq_wake_all(waitq_t *wq);

#endif /* _KERNEL_WAITQ_H */
