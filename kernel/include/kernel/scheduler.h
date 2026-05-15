#ifndef _KERNEL_SCHEDULER_H
#define _KERNEL_SCHEDULER_H

#include <stdint.h>

/* Maximum number of concurrently tracked tasks. */
#define SCHEDULER_MAX_TASKS  16

typedef enum {
    TASK_UNUSED  = 0,   /* slot is free                   */
    TASK_READY   = 1,   /* runnable, waiting for CPU time  */
    TASK_RUNNING = 2,   /* currently executing             */
    TASK_BLOCKED = 3,   /* waiting on I/O or an event      */
} task_state_t;

typedef struct task {
    uint32_t     esp;           /* saved kernel stack pointer (x86) */
    uint32_t     cr3;           /* page directory base -- future use  */
    task_state_t state;
    uint32_t     id;
    uint8_t      stack[4096];   /* per-task kernel stack             */
} task_t;

void     scheduler_initialize(void);
int      scheduler_create_task(void (*entry)(void));
task_t  *scheduler_get_current(void);
int      scheduler_next_index(void);   /* pure C: unit-testable */
void     scheduler_tick(void);
uint32_t scheduler_task_count(void);

/*
 * scheduler_yield -- voluntarily relinquish the CPU.
 *
 * In the kernel build this performs a real context switch to the next
 * PROC_READY process (via context_switch in boot.S).  The caller is
 * responsible for setting the current process's state before calling
 * (e.g. PROC_ZOMBIE for SYS_EXIT, PROC_BLOCKED for SYS_WAIT) so that
 * the scheduler does not re-pick it immediately.
 *
 * In the host/test build this is a no-op.
 */
void scheduler_yield(void);

#endif /* _KERNEL_SCHEDULER_H */
