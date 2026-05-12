/*
 * Quilon OS — Wait Queue Implementation (section 12.2)
 *
 * Three operations: sleep (block), wake_one, wake_all.
 *
 * waitq_sleep stack-allocates its own waitq_entry_t.  The cleanup path
 * (after scheduler_yield returns) removes the entry from the list in case
 * no wake call ran (e.g. scheduler_yield returned immediately because no
 * other process was runnable).  This makes the function safe to call in a
 * spin-like loop without accumulating stale list entries.
 */

#include <stddef.h>
#include <kernel/waitq.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>

void waitq_sleep(waitq_t *wq)
{
    if (!current_process) return;   /* no scheduler yet — caller's loop spins */

    waitq_entry_t entry = { .proc = current_process, .next = wq->head };
    wq->head = &entry;

    current_process->state = PROC_BLOCKED;
    scheduler_yield();

    /* Remove our entry from the list.  In the normal wake path
     * (waitq_wake_one / waitq_wake_all), the entry was already
     * detached and the process is PROC_READY.  If scheduler_yield
     * returned without switching (no other runnable process), the
     * entry is still in the list and we clean it up here.           */
    waitq_entry_t **pp = &wq->head;
    while (*pp) {
        if (*pp == &entry) { *pp = entry.next; break; }
        pp = &(*pp)->next;
    }
    current_process->state = PROC_RUNNING;
}

void waitq_wake_one(waitq_t *wq)
{
    if (!wq->head) return;
    waitq_entry_t *e = wq->head;
    wq->head = e->next;
    e->proc->state = PROC_READY;
}

void waitq_wake_all(waitq_t *wq)
{
    /* Detach the whole list atomically before iterating so that a
     * concurrent waitq_sleep (on another CPU or re-entrant IRQ) pushes
     * onto a fresh empty list rather than our in-progress one.         */
    waitq_entry_t *e = wq->head;
    wq->head = NULL;
    while (e) {
        waitq_entry_t *next = e->next;
        e->proc->state = PROC_READY;
        e = next;
    }
}
