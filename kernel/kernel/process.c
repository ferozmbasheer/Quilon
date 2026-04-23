/*
 * Quilon OS — Process Management (section 5.2 – 5.4)
 *
 * This file implements the process table, PCB allocation, round-robin
 * scheduling, and the process_launch() function used by the first-run
 * trampoline to enter ring-3.
 *
 * Sections implemented here:
 *   5.2 — Process Control Block and Process Table
 *   5.4 — process_launch() (supports 5.4 exit/wait lifecycle)
 *
 * Section 5.3 (context_switch assembly + scheduler_tick wiring) is in
 * boot.S and scheduler.c respectively.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <kernel/process.h>

#ifdef __is_kernel
#include <kernel/usermode.h>
#include <kernel/gdt.h>
/* process_first_run is an assembly label in boot.S */
extern void process_first_run(void);
#endif

/* ── Global state ─────────────────────────────────────────────────────────── */

process_t  process_table[PROCESS_MAX];
process_t *current_process = NULL;

/* ── Initialisation ───────────────────────────────────────────────────────── */

void process_init(void)
{
    for (int i = 0; i < PROCESS_MAX; i++) {
        process_table[i].state      = PROC_UNUSED;
        process_table[i].pid        = (uint32_t)i;
        process_table[i].parent_pid = 0;
        process_table[i].kernel_esp = 0;
        process_table[i].cr3        = 0;
        process_table[i].entry      = 0;
        process_table[i].exit_code  = 0;
        process_table[i].name[0]    = '\0';
    }
    current_process = NULL;
}

/* ── PCB allocation ───────────────────────────────────────────────────────── */

process_t *process_create(const char *name, uint32_t entry, uint32_t cr3)
{
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state != PROC_UNUSED)
            continue;

        process_t *p = &process_table[i];

        p->pid        = (uint32_t)i;
        p->parent_pid = current_process ? current_process->pid : 0;
        p->state      = PROC_READY;
        p->entry      = entry;
        p->cr3        = cr3;
        p->exit_code  = 0;

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
         * Layout (low address → high address, stack grows downward):
         *
         *   kernel_esp → [edi=0][esi=0][ebx=0][ebp=0][ret=process_first_run]
         *                  +0      +4     +8     +12    +16
         *
         * After pop edi/esi/ebx/ebp, ret pops the return address and jumps
         * to process_first_run, which calls process_launch().
         */
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

/* ── Lookup ───────────────────────────────────────────────────────────────── */

process_t *process_find(uint32_t pid)
{
    if (pid >= PROCESS_MAX)                        return NULL;
    if (process_table[pid].state == PROC_UNUSED)   return NULL;
    return &process_table[pid];
}

/* ── Round-robin scheduling helper ───────────────────────────────────────── */

/*
 * process_pick_next — find the next PROC_READY process after current_process.
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
    for (int i = 1; i <= PROCESS_MAX; i++) {
        int idx = (start + i) % PROCESS_MAX;
        if (process_table[idx].state == PROC_READY)
            return &process_table[idx];
    }
    return NULL;   /* no other runnable process */
}

/* ── First-run entry point ────────────────────────────────────────────────── */

#ifdef __is_kernel
/*
 * process_launch — called by the process_first_run assembly trampoline.
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
     * ring-3 → ring-0 transition (IRQ0, int $0x80, exceptions).
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
     * Load the TSS into TR (if not already done) and make the first 4 MiB
     * user-accessible so ring-3 code can execute.
     *
     * usermode_initialize() is idempotent (it checks a static flag), so
     * calling it on every first-run is safe.
     */
    usermode_initialize();

    /* Jump to ring 3 — never returns. */
    usermode_enter((void (*)(void))(uintptr_t)current_process->entry);

    __builtin_unreachable();
}
#else
/* Host/test build: process_launch is never called; provide a stub. */
void __attribute__((noreturn)) process_launch(void)
{
    while (1) { }
}
#endif /* __is_kernel */
