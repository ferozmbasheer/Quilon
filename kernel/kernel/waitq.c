/*
 * Quilon OS -- Wait Queue Implementation (section 12.2)
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
#include <stdint.h>
#include <kernel/waitq.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>

/*
 * Interrupt safety
 * ----------------
 * waitq_wake_one / waitq_wake_all run in IRQ context (IF=0): the keyboard
 * handler wakes kb_wq, etc.  waitq_sleep runs in thread context (IF=1) and is
 * preemptible.  The list mutation must therefore be atomic against IRQs, and --
 * crucially -- waitq_sleep must enqueue itself BEFORE an interrupt can fire,
 * or a wake that lands in the gap between "check predicate" and "enqueue" is
 * lost and the thread blocks forever (the classic lost-wakeup that froze the
 * desktop the moment a real key/mouse IRQ overlapped a blocking read).
 *
 * Fix: disable interrupts around every list mutation.  waitq_sleep keeps IRQs
 * off across the enqueue + state=BLOCKED, then restores the caller's interrupt
 * state immediately before yielding -- so the entry is guaranteed on the list
 * before any wake can run.  scheduler_yield performs the context switch with
 * the restored flags.
 */
#ifdef __is_kernel
static inline uint32_t wq_irq_off(void)
{
    uint32_t flags;
    asm volatile("pushf\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void wq_irq_restore(uint32_t flags)
{
    asm volatile("push %0\n\tpopf" :: "r"(flags) : "memory", "cc");
}
#else
static inline uint32_t wq_irq_off(void) { return 0; }
static inline void wq_irq_restore(uint32_t flags) { (void)flags; }
#endif

void waitq_sleep(waitq_t *wq)
{
    if (!current_process) return;   /* no scheduler yet -- caller's loop spins */

    uint32_t flags = wq_irq_off();

    waitq_entry_t entry = { .proc = current_process, .next = wq->head };
    wq->head = &entry;
    current_process->state = PROC_BLOCKED;

    /* Entry is now on the list and we are BLOCKED.  Restore interrupts and
     * yield: any wake from here on will correctly find and ready our entry. */
    wq_irq_restore(flags);
    scheduler_yield();

#ifdef __is_kernel
    /* scheduler_yield returns at once when nothing else is runnable.  We
     * used to fall straight back into the caller's predicate loop, so a
     * blocked process with nobody else to run spun the CPU in ring 0.
     * Instead, halt until the next interrupt (timer, keyboard, mouse...).
     * cli + re-check + "sti; hlt" closes the race with a wake that lands
     * between the check and the halt: sti only takes effect after the next
     * instruction, so the IRQ cannot slip in before hlt.  Only when the
     * caller had interrupts enabled -- otherwise nothing could wake us.
     * ROADMAP3 18.1. */
    if (flags & 0x200u) {
        asm volatile("cli" ::: "memory");
        if (current_process->state == PROC_BLOCKED)
            asm volatile("sti; hlt" ::: "memory");
        else
            asm volatile("sti" ::: "memory");
    }
#endif

    /* Remove our entry if still present (e.g. yield returned without a real
     * switch because no other process was runnable).  IRQs off so a wake
     * cannot mutate the list mid-walk. */
    flags = wq_irq_off();
    waitq_entry_t **pp = &wq->head;
    while (*pp) {
        if (*pp == &entry) { *pp = entry.next; break; }
        pp = &(*pp)->next;
    }
    current_process->state = PROC_RUNNING;
    wq_irq_restore(flags);
}

void waitq_wake_one(waitq_t *wq)
{
    uint32_t flags = wq_irq_off();
    if (wq->head) {
        waitq_entry_t *e = wq->head;
        wq->head = e->next;
        e->proc->state = PROC_READY;
    }
    wq_irq_restore(flags);
}

void waitq_wake_all(waitq_t *wq)
{
    uint32_t flags = wq_irq_off();
    /* Detach the whole list before iterating so a concurrent waitq_sleep
     * pushes onto a fresh empty list rather than our in-progress one. */
    waitq_entry_t *e = wq->head;
    wq->head = NULL;

    /* Walk with IRQs still off.  The entries live on the sleepers' stacks;
     * once a sleeper is READY (or its timeout fires -- ROADMAP3 18.1) a timer
     * IRQ could switch to it, it would return from waitq_sleep, and its entry
     * -- possibly the one we are about to follow -- would be gone. */
    while (e) {
        waitq_entry_t *next = e->next;
        e->proc->state = PROC_READY;
        e = next;
    }
    wq_irq_restore(flags);
}

/* -- "some input arrived" queue (ROADMAP3 18.1) -------------------------- */

waitq_t input_wq = WAITQ_INIT;

void input_notify(void)
{
    if (input_wq.head) waitq_wake_all(&input_wq);
}
