#include <stddef.h>
#include <stdint.h>
#include <kernel/scheduler.h>

#ifdef __is_kernel
#include <kernel/process.h>
#include <kernel/paging.h>
#include <kernel/gdt.h>
/* context_switch is defined in arch/i386/boot.S */
extern void context_switch(uint32_t *prev_esp_ptr, uint32_t next_esp_val);
#endif

static task_t   tasks[SCHEDULER_MAX_TASKS];
static int      current_task = 0;
static uint32_t task_count   = 0;

/* Reset all task slots and internal state. */
void scheduler_initialize(void)
{
    for (int i = 0; i < SCHEDULER_MAX_TASKS; i++) {
        tasks[i].state = TASK_UNUSED;
        tasks[i].id    = (uint32_t)i;
        tasks[i].esp   = 0;
        tasks[i].cr3   = 0;
    }
    current_task = 0;
    task_count   = 0;
}

/*
 * scheduler_create_task -- allocate a free task slot and set it up to
 * start executing at `entry` when first scheduled.
 *
 * Returns the task index on success, or -1 if all slots are full.
 *
 * Stack layout for a new task (top = highest address):
 *   [ entry point ]  ← the context-switch `ret` will pop this as EIP
 */
int scheduler_create_task(void (*entry)(void))
{
    for (int i = 0; i < SCHEDULER_MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            tasks[i].id    = (uint32_t)i;
            tasks[i].cr3   = 0;
            tasks[i].state = TASK_READY;

            uint32_t *sp = (uint32_t *)(tasks[i].stack + sizeof(tasks[i].stack));
            *(--sp) = (uint32_t)(uintptr_t)entry;
            tasks[i].esp = (uint32_t)(uintptr_t)sp;

            task_count++;
            return i;
        }
    }
    return -1;
}

/* Returns a pointer to the currently running task descriptor. */
task_t *scheduler_get_current(void)
{
    return &tasks[current_task];
}

/*
 * scheduler_next_index -- pure round-robin selection.
 *
 * Scans forward from the current task and returns the index of the first
 * READY or RUNNING task found.  Returns current_task if no other task is
 * runnable (including when there are no tasks at all).
 *
 * This function contains no architecture-specific code and is fully
 * unit-testable on the host.
 */
int scheduler_next_index(void)
{
    if (task_count == 0)
        return current_task;

    for (int i = 1; i <= SCHEDULER_MAX_TASKS; i++) {
        int idx = (current_task + i) % SCHEDULER_MAX_TASKS;
        if (tasks[idx].state == TASK_READY ||
            tasks[idx].state == TASK_RUNNING)
            return idx;
    }
    return current_task;
}

/* Returns the number of tasks that have been created (not freed). */
uint32_t scheduler_task_count(void)
{
    return task_count;
}

/*
 * scheduler_tick -- called from pit_tick() on every IRQ0 (100 Hz).
 *
 * In the kernel build, delegates to the process table for real preemptive
 * context switching (section 5.3).  The low-level swap is done by
 * context_switch() in boot.S, which saves/restores callee-saved registers
 * and the kernel stack pointer.
 *
 * In the host (test) build, falls back to the task_t round-robin table
 * without any hardware-specific code so unit tests remain runnable.
 */
void scheduler_tick(void)
{
#ifdef __is_kernel
    if (!current_process) return;

    process_t *next = process_pick_next();
    if (!next) return;   /* no other runnable process */

    /* Switch address space if the new process has a different PD. */
    if (next->cr3 && next->cr3 != current_process->cr3)
        paging_switch(next->cr3);

    /* Update TSS so ring-3 interrupts land on the new process's stack. */
    uint32_t kstack_top =
        (uint32_t)(uintptr_t)(next->kernel_stack + sizeof(next->kernel_stack));
    gdt_set_kernel_stack(kstack_top);

    process_t *prev = current_process;
    current_process = next;
    if (prev->state == PROC_RUNNING) prev->state = PROC_READY;
    next->state = PROC_RUNNING;

    /* Perform the actual stack swap.  After this call returns we are
     * executing on next's kernel stack.  For an existing process, the
     * return goes back through pit_tick -> irq0_handler -> irq0 -> iret.
     * For a new process, it goes to process_first_run (boot.S).        */
    context_switch(&prev->kernel_esp, next->kernel_esp);

#else  /* host / unit-test build */
    int next_idx = scheduler_next_index();
    if (next_idx == current_task) return;

    if (tasks[current_task].state == TASK_RUNNING)
        tasks[current_task].state = TASK_READY;
    int prev = current_task;
    current_task = next_idx;
    tasks[current_task].state = TASK_RUNNING;
    (void)prev;
#endif
}

/*
 * scheduler_yield -- voluntarily give up the CPU.
 *
 * Called by SYS_EXIT (to hand off after marking the process ZOMBIE) and
 * by SYS_WAIT (to sleep while waiting for a child).  Unlike scheduler_tick,
 * it does not emit the PIC EOI -- we are not inside an IRQ handler.
 *
 * In the kernel build: does a real context switch to the next READY process.
 * In the host build: no-op (no hardware, no real switching needed).
 */
void scheduler_yield(void)
{
#ifdef __is_kernel
    if (!current_process) return;

    process_t *next = process_pick_next();
    if (!next || next == current_process) return;

    if (next->cr3 && next->cr3 != current_process->cr3)
        paging_switch(next->cr3);

    uint32_t kstack_top =
        (uint32_t)(uintptr_t)(next->kernel_stack + sizeof(next->kernel_stack));
    gdt_set_kernel_stack(kstack_top);

    process_t *prev = current_process;
    current_process = next;
    /* NOTE: do NOT change prev->state here.  Callers (SYS_EXIT, SYS_WAIT)
     * have already set it to PROC_ZOMBIE or PROC_BLOCKED.               */
    next->state = PROC_RUNNING;

    context_switch(&prev->kernel_esp, next->kernel_esp);
#endif
}
