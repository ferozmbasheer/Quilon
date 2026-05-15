/*
 * Quilon OS -- Signal Implementation (section 6.4)
 *
 * signal_send() and signal_dispatch() implement the minimal signal subsystem
 * described in ROADMAP2.md section 6.4.
 *
 * Supported actions
 * -----------------
 * SIG_DFL: terminate the process (PROC_ZOMBIE) and wake the parent.
 * SIG_IGN: clear the pending bit, do nothing.
 * user fn:  TODO -- requires building a ring-3 signal stack frame and issuing
 *            SYS_SIGRETURN on return.  The infrastructure (pending_signals,
 *            signal_handlers[]) is already in place; delivery is the missing piece.
 *
 * Thread safety
 * -------------
 * IRQs may fire between reading pending_signals and clearing a bit.  The
 * bitmask operations are not atomic.  For a single-CPU kernel with IRQs
 * disabled during exception/syscall handlers this is safe.  An SMP kernel
 * would need compare-and-swap or interrupt masking.
 */

#include <stdint.h>
#include <stdio.h>

#include <kernel/signal.h>
#include <kernel/process.h>

#ifdef __is_kernel
#include <kernel/scheduler.h>
#endif

/* -- signal_send ----------------------------------------------------------- */

void signal_send(struct process *proc, int signum)
{
    if (!proc || signum < 0 || signum >= NSIG)
        return;

    proc->pending_signals |= (1u << (unsigned)signum);

#ifdef __is_kernel
    /* If the process is sleeping in SYS_WAIT, wake it so signal_dispatch()
     * can run on its next scheduling.                                     */
    if (proc->state == PROC_BLOCKED)
        proc->state = PROC_READY;
#endif
}

/* -- signal_dispatch ------------------------------------------------------- */

void signal_dispatch(void)
{
#ifdef __is_kernel
    if (!current_process) return;
    if (!current_process->pending_signals) return;

    /* Find the lowest-numbered pending signal. */
    uint32_t sigs = current_process->pending_signals;
    int signum = 0;
    while (signum < NSIG && !(sigs & (1u << (unsigned)signum)))
        signum++;

    if (signum >= NSIG) return;

    /* Clear the pending bit before dispatch so a re-entrant send is safe. */
    current_process->pending_signals &= ~(1u << (unsigned)signum);

    void (*handler)(int) = current_process->signal_handlers[signum];

    if (handler == SIG_IGN) {
        /* Ignore: bit already cleared, nothing more to do. */
        return;
    }

    if (handler == SIG_DFL) {
        /* Default action: terminate the process.
         *
         * SIGKILL and SIGSEGV both use the default terminate action.
         * The process moves to PROC_ZOMBIE, its exit code is set to
         * the negative signal number (matching Unix convention), and
         * the parent (if sleeping in SYS_WAIT) is woken.
         */
        printf("[signal] pid %d terminated by signal %d\r\n",
               (int)current_process->pid, signum);

        current_process->exit_code = -signum;
        current_process->state     = PROC_ZOMBIE;

        process_t *parent = process_find(current_process->parent_pid);
        if (parent && parent->state == PROC_BLOCKED)
            parent->state = PROC_READY;

        scheduler_yield();   /* never returns -- process is ZOMBIE */
        __builtin_unreachable();
    }

    /* User handler -- TODO: build ring-3 signal stack frame.
     * For now, treat any non-DFL, non-IGN handler like SIG_DFL so we
     * do not silently drop the signal.                                    */
    printf("[signal] pid %d: user handler for signal %d not yet supported"
           " - applying SIG_DFL\r\n",
           (int)current_process->pid, signum);

    current_process->exit_code = -signum;
    current_process->state     = PROC_ZOMBIE;

    process_t *parent = process_find(current_process->parent_pid);
    if (parent && parent->state == PROC_BLOCKED)
        parent->state = PROC_READY;

    scheduler_yield();
    __builtin_unreachable();

#endif /* __is_kernel */
}
