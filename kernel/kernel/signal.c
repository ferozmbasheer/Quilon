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
 * signal_send() runs from IRQ context (Ctrl-C in the keyboard handler) as
 * well as from syscalls, so the pending bitmask is updated with atomic
 * read-modify-write operations: a send can never be lost to a concurrent
 * dispatch clearing a different bit.
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

    /* An ignored signal is discarded at send time (POSIX semantics), so it
     * never wakes the process or interrupts a blocking read.  SIGKILL can't
     * be ignored. */
    if (signum != SIGKILL && proc->signal_handlers[signum] == SIG_IGN)
        return;

    __atomic_fetch_or(&proc->pending_signals, 1u << (unsigned)signum,
                      __ATOMIC_SEQ_CST);

#ifdef __is_kernel
    /* If the process is sleeping in SYS_WAIT, wake it so signal_dispatch()
     * can run on its next scheduling.                                     */
    if (proc->state == PROC_BLOCKED)
        proc->state = PROC_READY;
#endif
}

/* -- signal_pending / signal_set_handler ---------------------------------- */

int signal_pending(const struct process *proc)
{
    return proc && __atomic_load_n(&proc->pending_signals, __ATOMIC_SEQ_CST) != 0;
}

int signal_set_handler(struct process *proc, int signum, void (*handler)(int))
{
    if (!proc || signum <= 0 || signum >= NSIG || signum == SIGKILL)
        return -1;
    if (handler != SIG_DFL && handler != SIG_IGN)
        return -1;                       /* user handlers: not delivered yet */

    proc->signal_handlers[signum] = handler;
    if (handler == SIG_IGN)
        __atomic_fetch_and(&proc->pending_signals,
                           ~(1u << (unsigned)signum), __ATOMIC_SEQ_CST);
    return 0;
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
    __atomic_fetch_and(&current_process->pending_signals,
                       ~(1u << (unsigned)signum), __ATOMIC_SEQ_CST);

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

        /* Same cleanup as SYS_EXIT (owned fds, gfx surface, children) --
         * a crashing WM used to leave the screen stuck in graphics mode. */
        process_exit(-signum);
    }

    /* User handler -- TODO: build ring-3 signal stack frame.
     * For now, treat any non-DFL, non-IGN handler like SIG_DFL so we
     * do not silently drop the signal.                                    */
    printf("[signal] pid %d: user handler for signal %d not yet supported"
           " - applying SIG_DFL\r\n",
           (int)current_process->pid, signum);

    process_exit(-signum);

#endif /* __is_kernel */
}
