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
    uint32_t     cr3;           /* page directory base — future use  */
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

#endif /* _KERNEL_SCHEDULER_H */
