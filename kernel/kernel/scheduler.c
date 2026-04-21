#include <stddef.h>
#include <stdint.h>
#include <kernel/scheduler.h>

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
 * scheduler_create_task — allocate a free task slot and set it up to
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
 * scheduler_next_index — pure round-robin selection.
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
 * scheduler_tick — called from pit_tick() on every IRQ0.
 *
 * Selects the next READY task (round-robin) and updates task states.
 *
 * ── Note on preemptive context switching ─────────────────────────────────
 * Full preemption requires saving and restoring all CPU registers atomically
 * inside the interrupt frame.  The IRQ0 stub in boot.S would need to pass
 * a pointer to the saved register state so that modifying saved EIP here
 * causes `iret` to resume the new task.
 *
 * The ESP swap below is the minimal form; a complete implementation also
 * needs to coordinate EFLAGS, segment registers, and the initial stack
 * frame for newly created tasks.
 * ─────────────────────────────────────────────────────────────────────────
 */
void scheduler_tick(void)
{
    int next = scheduler_next_index();
    if (next == current_task)
        return;

    /* Demote the outgoing task so it re-enters the ready queue. */
    if (tasks[current_task].state == TASK_RUNNING)
        tasks[current_task].state = TASK_READY;

    int prev    = current_task;
    current_task = next;
    tasks[current_task].state = TASK_RUNNING;

#ifdef __is_kernel
    /* x86 context switch: save current stack pointer, load next. */
    asm volatile(
        "mov %%esp, %0\n"
        "mov %1, %%esp\n"
        : "=m"(tasks[prev].esp)
        : "m"(tasks[next].esp)
        : "memory"
    );
#else
    (void)prev;
#endif
}
