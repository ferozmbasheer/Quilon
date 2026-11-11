/*
 * Quilon OS -- Process Control Block and Process Table (section 5.2)
 *
 * A Process Control Block (PCB) is the kernel's record of one process.
 * Everything the kernel needs to suspend a process and resume it later
 * lives here: saved registers (via kernel_esp), page directory (cr3),
 * file descriptors, state, and PID.
 *
 * The process table is a fixed-size array of PCBs.  When a process exits,
 * its slot transitions to PROC_ZOMBIE until the parent calls wait(), at
 * which point it becomes PROC_UNUSED and can be reused.
 */

#ifndef _KERNEL_PROCESS_H
#define _KERNEL_PROCESS_H

#include <stdint.h>
#include <kernel/signal.h>   /* NSIG, SIG_DFL -- used in process_t below */
#include <kernel/vma.h>      /* vma_t, PROC_VMA_MAX -- demand paging (9.2) */

#define PROCESS_MAX        16    /* maximum concurrent processes               */

/* Per-process kernel stack (REVIEW.md G).  An overflow grows DOWN into the PCB
 * fields declared before the stack (name, signal handlers, std fds, heap_end,
 * cr3, state ...) and used to go unnoticed.  Now a canary at the low end is
 * checked on every context switch, and exit warns past 3/4 use.  Measured
 * high-water (ktest incl. ring-0 page faults inside syscalls, concurrent file
 * I/O, fork/exec/signals; desktop with terminals): ~1 KiB, so 4 KiB stays. */
#define PROCESS_KSTACK_SIZE 4096u

/* Lowest word of every kernel stack: checked on each context switch. */
#define KSTACK_CANARY      0x5AFEC0DEu
/* Fill pattern for the rest of the stack, for high-water measurement. */
#define KSTACK_FILL        0xA5u
#define PROCESS_NAME_LEN   16    /* max process name length (including NUL)    */

/*
 * proc_state_t -- lifecycle states of a process.
 *
 * PROC_UNUSED:  Slot is free; may be reused by process_create().
 * PROC_RUNNING: Currently executing on the CPU.
 * PROC_READY:   Runnable, waiting for the scheduler to pick it.
 * PROC_BLOCKED: Sleeping in wait() until a child calls exit().
 * PROC_ZOMBIE:  Called exit(); resources held until parent calls wait().
 */
typedef enum {
    PROC_UNUSED  = 0,
    PROC_RUNNING = 1,
    PROC_READY   = 2,
    PROC_BLOCKED = 3,
    PROC_ZOMBIE  = 4,
} proc_state_t;

/*
 * process_t -- Process Control Block.
 *
 * Every live process has one PCB in process_table[].  The fields used
 * for context switching are kernel_esp and cr3:
 *
 *   kernel_esp  -- saved kernel stack pointer.  context_switch() writes
 *                 the current ESP here when suspending the process, and
 *                 reads it back when resuming.  The value points inside
 *                 kernel_stack[].
 *
 *   cr3         -- physical address of this process's page directory.
 *                 Loaded into CR3 on every context switch so the MMU
 *                 sees the correct virtual-to-physical mapping.
 *
 * See section 5.3 of ROADMAP2.md for the context-switch protocol.
 */
typedef struct process {
    uint32_t     pid;              /* unique process ID (= slot index)          */
    uint32_t     parent_pid;       /* parent's PID (0 for the initial process)  */
    uint32_t     thread_group;     /* 0 = process leader; leader pid for threads */
    proc_state_t state;            /* current lifecycle state                   */

    /*
     * Saved kernel-mode stack pointer.
     *
     * context_switch(prev, next) saves the old ESP here and loads the new
     * ESP from next->kernel_esp.  The register save area pushed by
     * context_switch (edi, esi, ebx, ebp + return address) lives at this
     * location in kernel_stack[].
     */
    uint32_t     kernel_esp;

    /*
     * Physical address of this process's page directory.
     *
     * Created by paging_create_address_space() and written into CR3 on
     * every context switch.  Each process has its own PD so that user
     * pages at the same virtual address (e.g. 0x400000 for ELF segments)
     * are physically distinct.
     */
    uint32_t     cr3;

    /*
     * Virtual entry point of the user program.
     *
     * Stored from the ELF header's e_entry field by process_create().
     * Used by process_launch() (called from the process_first_run
     * assembly trampoline) to do the iret into ring 3.
     */
    uint32_t     entry;

    /* Exit status, written by SYS_EXIT, read by SYS_WAIT.             */
    int          exit_code;

    /*
     * User heap (section 6.3 -- SYS_SBRK).
     *
     * heap_end tracks the current program break (top of the heap).
     * Initialised to 0; the first SYS_SBRK call sets it to USER_HEAP_START
     * (0x800000) and then extends it by the requested increment.
     *
     * Each page in [old_break, new_break) is allocated by pmm_alloc_page()
     * and mapped by paging_map_page_alloc() with PAGE_USER | PAGE_WRITABLE.
     */
    uint32_t     heap_end;          /* current program break; 0 = uninitialised */

    int          stdin_fd;            /* override for fd 0: pipe fd or -1     */
    int          stdout_fd;           /* override for fd 1/2: pipe fd or -1   */
    int          owns_std_fds;        /* 1 = close stdin/stdout_fd on exit.
                                       * Set only for a process the fds were
                                       * explicitly handed to (exec_redir, e.g.
                                       * the gterm shell).  A plain-exec child
                                       * that merely INHERITED them must NOT
                                       * close them -- they belong to an
                                       * ancestor that is still using them.   */

    /*
     * Signal state (section 6.4).
     *
     * pending_signals: bitmask of signals awaiting delivery.
     *   Bit n is set by signal_send(proc, n).
     *   Bit n is cleared by signal_dispatch() when the signal is handled.
     *
     * signal_handlers[n]: how to handle signal n.
     *   SIG_DFL (0): default action (usually terminate).
     *   SIG_IGN (1): ignore.
     *   other:       ring-3 user handler function pointer (delivery TBD).
     */
    uint32_t     pending_signals;
    void       (*signal_handlers[NSIG])(int);

    /*
     * Foreground job (ROADMAP3 17.0.1, Ctrl-C).  A shell sets this to the
     * child it is waiting for (SYS_SETFG) and back to -1 afterwards.  Ctrl-C
     * follows the fg_child chain down from a terminal's shell and sends
     * SIGINT to the deepest process -- see process_intr_target().
     * -1 = none (PIDs are slot indices, so 0 is a valid PID).
     */
    int          fg_child;

    char         name[PROCESS_NAME_LEN];

    /*
     * Per-process kernel stack -- one 4-KiB page, 16-byte aligned.
     *
     * The stack grows downward from kernel_stack + sizeof(kernel_stack).
     * process_create() builds an initial context_switch frame at the top
     * of this array and points kernel_esp at it, so that when the
     * scheduler first picks this process and calls context_switch, the
     * subsequent ret lands in process_first_run (boot.S trampoline).
     */
    uint8_t      kernel_stack[PROCESS_KSTACK_SIZE] __attribute__((aligned(16)));

    /*
     * Virtual Memory Areas (section 9.2 -- demand paging).
     *
     * Each VMA records a contiguous virtual address range [start, end) and
     * its permission flags.  The page-fault handler uses this table to decide
     * whether a not-present fault is a genuine segfault or a demand-page
     * that should be satisfied by allocating a zero physical page.
     *
     * Populated by elf_load_into() (one VMA per PT_LOAD segment + one for the
     * stack) and extended by SYS_SBRK (heap VMA grows as malloc calls sbrk).
     */
    vma_t        vmas[PROC_VMA_MAX];
} process_t;

/* -- Global state ----------------------------------------------------------- */

extern process_t  process_table[PROCESS_MAX];  /* all PCBs               */
extern process_t *current_process;             /* the running process     */

/* -- API -------------------------------------------------------------------- */

/*
 * process_init -- initialise the process table.
 *
 * Marks all slots PROC_UNUSED and clears current_process.
 * Call once during kernel startup before creating any processes.
 */
void process_init(void);

/*
 * process_create -- allocate and initialise a new process slot.
 *
 *   name  -- short human-readable label (e.g. ELF filename).
 *   entry -- virtual address of the user program's first instruction (e_entry).
 *   cr3   -- physical address of the new page directory, as returned by
 *            paging_create_address_space().
 *
 * Sets up a fake context_switch frame on kernel_stack[] so that the
 * very first call to context_switch with this process as `next` will
 * jump to the process_first_run assembly trampoline.
 *
 * Returns a pointer to the new PCB in PROC_READY state, or NULL if all
 * PROCESS_MAX slots are occupied.
 */
process_t *process_create(const char *name, uint32_t entry, uint32_t cr3);

/*
 * process_find -- look up a process by PID.
 *
 * Returns a pointer to the PCB, or NULL if the PID is out of range or
 * the slot is PROC_UNUSED.
 */
process_t *process_find(uint32_t pid);

/*
 * process_group_leader -- return the address-space owner for p.
 *
 * For a normal process this is p itself.  For a CLONE_VM thread (thread_group
 * != 0) it is the thread-group leader, which owns the shared program break
 * (heap_end) and VMA table.  Falls back to p if the leader has been reaped.
 */
process_t *process_group_leader(process_t *p);

/*
 * process_pick_next -- round-robin scheduler helper.
 *
 * Scans the process table for the next PROC_READY slot after
 * current_process and returns it.  Returns NULL if no other runnable
 * process exists.
 *
 * Pure C with no architecture-specific code -- fully unit-testable on
 * the host.
 */
process_t *process_pick_next(void);

/*
 * process_launch -- enter user mode for the current process.
 *
 * Called by the process_first_run assembly trampoline in boot.S on a
 * process's very first scheduling.  Updates the TSS kernel stack pointer
 * so that any ring-3 exception uses this process's kernel_stack[], then
 * calls usermode_initialize() and usermode_enter(entry).
 *
 * Never returns.
 */
void process_launch(void) __attribute__((noreturn));

/* -- Exit / reap lifecycle (REVIEW.md C, D, E) ------------------------------
 *
 *   running --process_mark_exited--> PROC_ZOMBIE --process_reap--> PROC_UNUSED
 *
 * Every way a process can die (SYS_EXIT, a fatal signal) goes through
 * process_exit -> process_mark_exited, so the cleanup cannot diverge again.
 */

/* parent_pid of a process whose parent has exited: nobody will wait() for it,
 * so it is reaped automatically once it becomes a zombie. */
#define PROC_NO_PARENT  0xFFFFFFFFu

/*
 * process_mark_exited -- turn p into a zombie and release what it holds
 * that does not need its address space: closes the pipe fds it owns, gives
 * up the graphics surface, reaps its zombie children and reparents its live
 * ones to PROC_NO_PARENT, and wakes a parent blocked in wait().
 * Does not switch away -- see process_exit.
 */
void process_mark_exited(process_t *p, int code);

/*
 * process_reap -- free a zombie's slot and (unless a CLONE_VM sibling still
 * runs on it) its whole address space.  Must not be called on the process
 * whose address space is loaded in CR3, i.e. never on current_process.
 */
void process_reap(process_t *p);

/* Reap every zombie nobody will wait() for (parent gone).  Called before
 * allocating a slot, so orphans never pin the table or their memory. */
void process_reap_orphans(void);

/* May `waiter` wait() for `child`?  Its parent, or a thread in the parent's
 * thread group (pthread_join from a sibling thread). */
int process_is_waitable_by(const process_t *child, const process_t *waiter);

/* -- Ctrl-C / foreground jobs (ROADMAP3 17.0.1) ----------------------------- */

/* PID of the shell that owns the text console (the keyboard), or -1.  Set by
 * kmain when it launches SHELL.ELF. */
extern int console_shell_pid;

/*
 * process_intr_target -- who should Ctrl-C interrupt in root's terminal?
 *
 * Follows fg_child links down from root (each link must point at a live,
 * non-zombie child of the previous process) and returns the deepest process,
 * or NULL when the Ctrl-C should be passed on as an ordinary 0x03 byte:
 *   - root has no foreground job (the shell is at its prompt), or
 *   - the deepest process ignores SIGINT (the WM, which forwards Ctrl-C to
 *     the focused terminal itself).
 */
process_t *process_intr_target(process_t *root);

/* Ctrl-C on the text console (keyboard IRQ hook): SIGINT the console shell's
 * foreground job.  Returns 1 if a signal was sent, 0 otherwise. */
int process_console_interrupt(void);

/* Kernel-stack guard (REVIEW.md G).
 *   process_stack_intact -- is the canary at the stack's low end untouched?
 *   process_stack_used   -- high-water mark in bytes: how deep the stack has
 *                           ever grown (bytes no longer holding KSTACK_FILL). */
int      process_stack_intact(const process_t *p);
uint32_t process_stack_used(const process_t *p);

/* Exit the current process: process_mark_exited + switch away for good. */
void process_exit(int code) __attribute__((noreturn));

#ifndef __is_kernel
/* Host-test observability: what the lifecycle code would have released. */
extern uint32_t process_test_released_cr3[16];
extern int      process_test_released_count;
extern int      process_test_closed_fds[16];
extern int      process_test_closed_count;
#endif

#endif /* _KERNEL_PROCESS_H */
