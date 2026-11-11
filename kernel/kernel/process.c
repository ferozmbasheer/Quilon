/*
 * Quilon OS -- Process Management (section 5.2 – 5.4)
 *
 * This file implements the process table, PCB allocation, round-robin
 * scheduling, and the process_launch() function used by the first-run
 * trampoline to enter ring-3.
 *
 * Sections implemented here:
 *   5.2 -- Process Control Block and Process Table
 *   5.4 -- process_launch() (supports 5.4 exit/wait lifecycle)
 *
 * Section 5.3 (context_switch assembly + scheduler_tick wiring) is in
 * boot.S and scheduler.c respectively.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <kernel/process.h>
#include <kernel/vma.h>

#include <kernel/vfs.h>       /* VFS_FD_BASE */

#ifdef __is_kernel
#include <kernel/usermode.h>
#include <kernel/gdt.h>
#include <kernel/paging.h>
#include <kernel/scheduler.h>
#include <kernel/syscall.h>   /* gfx_release_owner */
/* process_first_run is an assembly label in boot.S */
extern void process_first_run(void);
#endif

/* -- Global state ----------------------------------------------------------- */

process_t  process_table[PROCESS_MAX];
process_t *current_process = NULL;

/* -- Initialisation --------------------------------------------------------- */

void process_init(void)
{
    for (int i = 0; i < PROCESS_MAX; i++) {
        process_table[i].state           = PROC_UNUSED;
        process_table[i].pid             = (uint32_t)i;
        process_table[i].parent_pid      = 0;
        process_table[i].thread_group    = 0;
        process_table[i].kernel_esp      = 0;
        process_table[i].cr3             = 0;
        process_table[i].entry           = 0;
        process_table[i].exit_code       = 0;
        process_table[i].heap_end        = 0;
        process_table[i].stdin_fd        = -1;
        process_table[i].stdout_fd       = -1;
        process_table[i].owns_std_fds    = 0;
        process_table[i].pending_signals = 0;
        process_table[i].name[0]         = '\0';
        for (int s = 0; s < NSIG; s++)
            process_table[i].signal_handlers[s] = SIG_DFL;
        vma_init(process_table[i].vmas, PROC_VMA_MAX);
    }
    current_process = NULL;
}

/* -- PCB allocation --------------------------------------------------------- */

process_t *process_create(const char *name, uint32_t entry, uint32_t cr3)
{
    /* Orphaned zombies would otherwise hold slots (and memory) forever. */
    process_reap_orphans();

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state != PROC_UNUSED)
            continue;

        process_t *p = &process_table[i];

        p->pid             = (uint32_t)i;
        p->parent_pid      = current_process ? current_process->pid : 0;
        p->thread_group    = 0;
        p->state           = PROC_READY;
        p->entry           = entry;
        p->cr3             = cr3;
        p->exit_code       = 0;
        p->heap_end        = 0;
        p->stdin_fd        = -1;
        p->stdout_fd       = -1;
        p->owns_std_fds    = 0;
        p->pending_signals = 0;
        p->fg_child        = -1;
        for (int s = 0; s < NSIG; s++)
            p->signal_handlers[s] = SIG_DFL;
        vma_init(p->vmas, PROC_VMA_MAX);

        /* Copy name (bounded, always NUL-terminated) */
        int j;
        for (j = 0; j < PROCESS_NAME_LEN - 1 && name && name[j]; j++)
            p->name[j] = name[j];
        p->name[j] = '\0';

        /*
         * Build the initial kernel stack frame for context_switch().
         *
         * context_switch(prev_esp_ptr, next_esp_val) pushes four callee-saved
         * registers onto the stack, then saves the resulting ESP into
         * *prev_esp_ptr.  When it restores, it loads next_esp_val into ESP
         * and does: pop edi; pop esi; pop ebx; pop ebp; ret.
         *
         * For a brand-new process, we pre-fill those five words at the top
         * of kernel_stack[] so that the ret lands in process_first_run.
         *
         * Layout (low address -> high address, stack grows downward):
         *
         *   kernel_esp -> [edi=0][esi=0][ebx=0][ebp=0][ret=process_first_run]
         *                  +0      +4     +8     +12    +16
         *
         * After pop edi/esi/ebx/ebp, ret pops the return address and jumps
         * to process_first_run, which calls process_launch().
         */
        /* Guard + high-water pattern (REVIEW.md G): canary in the lowest
         * word, fill pattern everywhere else. */
        memset(p->kernel_stack, KSTACK_FILL, sizeof(p->kernel_stack));
        uint32_t canary = KSTACK_CANARY;
        memcpy(p->kernel_stack, &canary, sizeof canary);

        uint32_t *sp = (uint32_t *)(p->kernel_stack + sizeof(p->kernel_stack));
#ifdef __is_kernel
        *(--sp) = (uint32_t)(uintptr_t)process_first_run;  /* ret address   */
#else
        *(--sp) = 0;   /* host build: trampoline not needed                  */
#endif
        *(--sp) = 0;   /* saved ebp */
        *(--sp) = 0;   /* saved ebx */
        *(--sp) = 0;   /* saved esi */
        *(--sp) = 0;   /* saved edi */
        p->kernel_esp = (uint32_t)(uintptr_t)sp;

        return p;
    }
    return NULL;   /* all PROCESS_MAX slots are occupied */
}

/* -- Lookup ----------------------------------------------------------------- */

process_t *process_find(uint32_t pid)
{
    if (pid >= PROCESS_MAX)                        return NULL;
    if (process_table[pid].state == PROC_UNUSED)   return NULL;
    return &process_table[pid];
}

process_t *process_group_leader(process_t *p)
{
    if (!p) return NULL;
    /* thread_group == 0 means p is itself the leader (a normal process).
     * Otherwise it holds the leader's PID; all CLONE_VM threads in one address
     * space share the leader's heap break and VMA table. */
    if (p->thread_group == 0)
        return p;
    process_t *leader = process_find(p->thread_group);
    return leader ? leader : p;   /* fall back to self if leader already reaped */
}

/* -- Exit / reap lifecycle (REVIEW.md C, D, E) ----------------------------- */

#ifdef __is_kernel
static void release_address_space(uint32_t cr3)
{
    paging_destroy_address_space((uint32_t *)(uintptr_t)cr3);
}
static void close_fd(int fd) { vfs_close(fd); }
#else
uint32_t process_test_released_cr3[16];
int      process_test_released_count;
int      process_test_closed_fds[16];
int      process_test_closed_count;
static void release_address_space(uint32_t cr3)
{
    if (process_test_released_count < 16)
        process_test_released_cr3[process_test_released_count] = cr3;
    process_test_released_count++;
}
static void close_fd(int fd)
{
    if (process_test_closed_count < 16)
        process_test_closed_fds[process_test_closed_count] = fd;
    process_test_closed_count++;
}
#endif

/* -- Kernel-stack guard (REVIEW.md G) --------------------------------------- */

/* Warn at exit when a process came within 1/4 of overflowing. */
#define KSTACK_WARN_BYTES (PROCESS_KSTACK_SIZE * 3u / 4u)

int process_stack_intact(const process_t *p)
{
    if (!p) return 0;
    uint32_t word;
    memcpy(&word, p->kernel_stack, sizeof word);
    return word == KSTACK_CANARY;
}

uint32_t process_stack_used(const process_t *p)
{
    if (!p) return 0;
    /* Scan up from just above the canary to the first byte the stack has
     * touched; everything from there to the top has been used. */
    uint32_t i = sizeof(uint32_t);
    while (i < sizeof(p->kernel_stack) && p->kernel_stack[i] == KSTACK_FILL)
        i++;
    return (uint32_t)sizeof(p->kernel_stack) - i;
}

/* Is cr3 still the address space of some live (non-UNUSED) slot other than p?
 * True for a CLONE_VM thread group until its last member is reaped. */
static int cr3_in_use_by_other(const process_t *p, uint32_t cr3)
{
    for (int i = 0; i < PROCESS_MAX; i++) {
        const process_t *q = &process_table[i];
        if (q != p && q->state != PROC_UNUSED && q->cr3 == cr3)
            return 1;
    }
    return 0;
}

void process_reap(process_t *p)
{
    if (!p || p->state == PROC_UNUSED) return;
    uint32_t cr3 = p->cr3;
    p->state = PROC_UNUSED;
    p->cr3   = 0;
    if (cr3 && !cr3_in_use_by_other(p, cr3))
        release_address_space(cr3);
}

void process_reap_orphans(void)
{
    for (int i = 0; i < PROCESS_MAX; i++) {
        process_t *q = &process_table[i];
        if (q->state != PROC_ZOMBIE || q == current_process) continue;
        if (q->parent_pid == PROC_NO_PARENT || q->parent_pid == q->pid ||
            !process_find(q->parent_pid))
            process_reap(q);
    }
}

int process_is_waitable_by(const process_t *child, const process_t *waiter)
{
    if (!child || !waiter || child == waiter) return 0;
    if (child->parent_pid == waiter->pid) return 1;
    /* pthread_join from a sibling: the child's parent is in our group. */
    const process_t *parent = process_find(child->parent_pid);
    return parent &&
           process_group_leader((process_t *)parent) ==
           process_group_leader((process_t *)waiter);
}

/* -- Ctrl-C / foreground jobs (ROADMAP3 17.0.1) ---------------------------- */

int console_shell_pid = -1;

process_t *process_intr_target(process_t *root)
{
    if (!root) return NULL;

    process_t *p = root;
    /* Bounded: a chain can't be longer than the table (guards a cycle made
     * of stale links). */
    for (int depth = 0; depth < PROCESS_MAX && p->fg_child >= 0; depth++) {
        process_t *c = process_find((uint32_t)p->fg_child);
        /* A stale link (the job exited, or its PID was recycled by a process
         * that is not our child) ends the chain. */
        if (!c || c == p || c->state == PROC_ZOMBIE || c->parent_pid != p->pid)
            break;
        p = c;
    }

    if (p == root) return NULL;                         /* shell at its prompt */
    if (p->signal_handlers[SIGINT] == SIG_IGN) return NULL;  /* e.g. the WM */
    return p;
}

#ifdef __is_kernel
/* Kernel-only: the host tests that link process.c don't all link signal.c. */
int process_console_interrupt(void)
{
    if (console_shell_pid < 0) return 0;
    process_t *t = process_intr_target(process_find((uint32_t)console_shell_pid));
    if (!t) return 0;
    signal_send(t, SIGINT);
    return 1;
}
#endif

void process_mark_exited(process_t *p, int code)
{
    if (!p) return;

    /* Close the pipe ends this process OWNS (handed over by exec_redir).  A
     * plain-exec child merely inherited them from an ancestor still using
     * them, so it must not close them. */
    if (p->owns_std_fds) {
        if (p->stdin_fd >= VFS_FD_BASE)
            close_fd(p->stdin_fd);
        if (p->stdout_fd >= VFS_FD_BASE && p->stdout_fd != p->stdin_fd)
            close_fd(p->stdout_fd);
    }
    p->stdin_fd     = -1;
    p->stdout_fd    = -1;
    p->owns_std_fds = 0;

#ifdef __is_kernel
    /* If the WM dies -- even by SIGSEGV -- free the screen for a new startx. */
    gfx_release_owner(p->pid);

    /* Early warning before a kernel stack overflow (REVIEW.md G). */
    uint32_t used = process_stack_used(p);
    if (used > KSTACK_WARN_BYTES)
        printf("[kernel] pid %d used %u of %u bytes of kernel stack\r\n",
               (int)p->pid, (unsigned)used, (unsigned)PROCESS_KSTACK_SIZE);
#endif

    p->exit_code = code;
    p->state     = PROC_ZOMBIE;

    /* Children: zombies can never be waited for now -- reap them; live ones
     * are orphaned and will be reaped as soon as they exit. */
    for (int i = 0; i < PROCESS_MAX; i++) {
        process_t *q = &process_table[i];
        if (q == p || q->state == PROC_UNUSED || q->parent_pid != p->pid)
            continue;
        if (q->state == PROC_ZOMBIE) process_reap(q);
        else                         q->parent_pid = PROC_NO_PARENT;
    }

    /* Wake the parent if it is sleeping in SYS_WAIT. */
    process_t *parent = (p->parent_pid != p->pid) ? process_find(p->parent_pid)
                                                  : NULL;
    if (parent && parent->state == PROC_BLOCKED)
        parent->state = PROC_READY;
}

#ifdef __is_kernel
void process_exit(int code)
{
    process_mark_exited(current_process, code);
    scheduler_yield();                  /* never resumes: we are a zombie */
    for (;;) asm volatile("hlt");       /* nothing else runnable */
}
#else
void process_exit(int code)
{
    process_mark_exited(current_process, code);
    while (1) { }
}
#endif

/* -- Round-robin scheduling helper ----------------------------------------- */

/*
 * process_pick_next -- find the next PROC_READY process after current_process.
 *
 * Scans forward (wrapping around) from the slot after current_process.
 * Returns the first PROC_READY entry found, or NULL if none exists.
 *
 * This function has no architecture-specific code and is unit-testable
 * on the host.
 */
process_t *process_pick_next(void)
{
    if (!current_process) return NULL;

    int start = (int)(current_process - process_table);
    for (int i = 1; i < PROCESS_MAX; i++) {
        int idx = (start + i) % PROCESS_MAX;
        if (process_table[idx].state == PROC_READY)
            return &process_table[idx];
    }
    return NULL;   /* no other runnable process */
}

/* -- First-run entry point -------------------------------------------------- */

#ifdef __is_kernel
/*
 * process_launch -- called by the process_first_run assembly trampoline.
 *
 * At this point, current_process has been set to the new process by the
 * scheduler.  We update the TSS kernel stack pointer so that any ring-3
 * interrupt (IRQ, syscall) will switch to this process's kernel_stack[],
 * then we call usermode_initialize() and usermode_enter() to iret into
 * ring 3 at current_process->entry.
 *
 * This function never returns.
 */
void __attribute__((noreturn)) process_launch(void)
{
    /*
     * Tell the CPU which kernel stack to use when this process causes a
     * ring-3 -> ring-0 transition (IRQ0, int $0x80, exceptions).
     *
     * The top of the kernel stack is kernel_stack[] + sizeof(kernel_stack[]).
     * We point the TSS esp0 there so that interrupts push the saved ring-3
     * state (SS, ESP, EFLAGS, CS, EIP) onto our private per-process stack
     * and not onto the shared boot stack.
     */
    uint32_t kstack_top =
        (uint32_t)(uintptr_t)(current_process->kernel_stack +
                              sizeof(current_process->kernel_stack));
    gdt_set_kernel_stack(kstack_top);

    /*
     * Load the TSS into TR (once only -- ltr faults if called twice;
     * usermode_initialize() guards it with a static flag).
     *
     * It deliberately grants NO user access to the identity map or the
     * kernel-high window: it used to, which let every exec'd process read
     * and write kernel memory (REVIEW.md H).  An ELF process only touches its
     * own pages (text/data at 0x400000+, heap, stack below USER_STACK_TOP).
     */
    usermode_initialize();

    /* Jump to ring 3 using the per-process stack at USER_STACK_TOP.
     * elf_load_into() mapped a private 4-KiB page at USER_STACK_TOP-PAGE_SIZE
     * in the process's page directory, so this virtual address is valid.   */
    usermode_enter_esp((void (*)(void))(uintptr_t)current_process->entry,
                       USER_STACK_TOP);

    __builtin_unreachable();
}
#else
/* Host/test build: process_launch is never called; provide a stub. */
void __attribute__((noreturn)) process_launch(void)
{
    while (1) { }
}
#endif /* __is_kernel */
