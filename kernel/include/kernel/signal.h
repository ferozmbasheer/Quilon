/*
 * Quilon OS -- Signal Definitions (section 6.4)
 *
 * Signals are asynchronous notifications sent to a process.  A bitmask in
 * each PCB (process_t.pending_signals) tracks which signals are outstanding.
 *
 * Delivery model
 * --------------
 * signal_send()    -- posts a signal to a process (sets a bit, wakes if sleeping);
 *                     a signal the process ignores (SIG_IGN) is discarded.
 * signal_dispatch() -- checks pending_signals for current_process and acts:
 *                      SIG_DFL: terminate (PROC_ZOMBIE) and wake the parent.
 *                      SIG_IGN: clear the bit, do nothing.
 *                      user handler: TODO (requires a sigreturn stack frame).
 *
 * signal_dispatch() is called:
 *   • At the tail of syscall_handler (after every system call).
 *   • From exception_handler after sending SIGSEGV on a ring-3 page fault.
 *
 * Relationship to section 6.4 of ROADMAP2.md
 * -------------------------------------------
 * Subsections implemented here:
 *   • pending_signals bitmask (in process_t -- see process.h)
 *   • signal_handlers array   (in process_t -- see process.h)
 *   • signal_send()
 *   • signal_dispatch()       (SIG_DFL + SIG_IGN; user-handler delivery TBD)
 *   • Page fault -> SIGSEGV   (in exceptions.c)
 */

#ifndef _KERNEL_SIGNAL_H
#define _KERNEL_SIGNAL_H

#include <stdint.h>

/* -- Number of signals ----------------------------------------------------- */

#define NSIG        32      /* total signal slots (bitmask width)             */

/* -- Signal numbers -------------------------------------------------------- */

#define SIGINT       2      /* interrupt from the keyboard (Ctrl-C)            */
#define SIGKILL      9      /* terminate unconditionally -- cannot be ignored  */
#define SIGSEGV     11      /* invalid memory reference (page fault in ring 3)*/
#define SIGTERM     15      /* polite termination request (kill's default)     */
#define SIGCHLD     17      /* child stopped or terminated                    */

/* -- Pseudo-handler constants ---------------------------------------------- */

/*
 * SIG_DFL -- perform the default action for the signal.
 * For most signals in Quilon the default action is to terminate the process.
 *
 * SIG_IGN -- ignore the signal entirely.
 *
 * These are stored in process_t.signal_handlers[].  Any other value is
 * treated as a user-space function pointer (ring-3 handler).
 * User-handler delivery requires building a signal stack frame and is
 * marked TODO in signal.c.
 */
#define SIG_DFL   ((void (*)(int))0)   /* default action   */
#define SIG_IGN   ((void (*)(int))1)   /* ignore           */

/* -- Forward declaration --------------------------------------------------- */
/* Avoids a circular include: signal.h -> process.h -> signal.h.               */
struct process;

/* -- API ------------------------------------------------------------------- */

/*
 * signal_send -- post signal signum to proc.
 *
 * Sets bit (signum) in proc->pending_signals.  If the process is currently
 * PROC_BLOCKED (sleeping in SYS_WAIT), it is promoted to PROC_READY so that
 * the scheduler will run it and signal_dispatch() can deliver the signal.
 *
 * signum must be in [0, NSIG).  Out-of-range values are silently ignored.
 *
 * Safe to call from any kernel context (exception handler, IRQ handler, etc.).
 */
void signal_send(struct process *proc, int signum);

/*
 * signal_pending -- 1 if proc has a signal awaiting delivery.
 *
 * Blocking kernel loops (keyboard read, pipe read/write, SYS_WAIT) check this
 * after every wake-up and bail out, so the syscall returns and
 * signal_dispatch() can act.  Ignored signals are never pending (signal_send
 * discards them), so this never aborts a read for a signal that would be
 * thrown away anyway.
 */
int signal_pending(const struct process *proc);

/*
 * signal_set_handler -- SYS_SIGNAL: set proc's disposition for signum.
 *
 * Only SIG_DFL and SIG_IGN are accepted (user handlers are not delivered
 * yet, so accepting one would silently mean "terminate").  SIGKILL cannot be
 * changed.  Setting SIG_IGN also discards an already-pending instance.
 * Returns 0 on success, -1 on a bad signal number or handler.
 */
int signal_set_handler(struct process *proc, int signum, void (*handler)(int));

/*
 * signal_dispatch -- deliver one pending signal to the current process.
 *
 * Examines current_process->pending_signals.  For the first set bit:
 *
 *   handler == SIG_IGN: clear the bit and return (signal ignored).
 *   handler == SIG_DFL: terminate the process (PROC_ZOMBIE), wake the parent,
 *                       and call scheduler_yield().  Does not return.
 *   other value:        TODO -- build ring-3 signal frame + SYS_SIGRETURN stub.
 *
 * Called after every system call and after sending SIGSEGV from exception.c.
 * Does nothing if current_process is NULL or has no pending signals.
 */
void signal_dispatch(void);

#endif /* _KERNEL_SIGNAL_H */
