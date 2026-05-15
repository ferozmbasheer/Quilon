/*
 * Quilon OS -- PIT and Scheduler Unit Tests
 *
 * Compiled with the native host gcc -- no cross-compiler or QEMU needed.
 *
 * What is tested here:
 *   PIT      -- pit_divisor() is a pure inline math function in pit.h;
 *               it requires no hardware and is fully testable on the host.
 *
 *   Scheduler -- scheduler.c contains only plain C (no x86 asm on the host
 *               because the ESP swap is guarded by #ifdef __is_kernel).
 *               All task-management and round-robin logic is testable.
 *
 * Build & run:  make  (from tests/)
 */

#include "framework.h"

#include <stdint.h>
#include <stddef.h>

#include <kernel/pit.h>
#include <kernel/scheduler.h>

/* =======================================================================
 * PIT -- divisor math
 * The PIT fires IRQ0 at  PIT_BASE_HZ / divisor  Hz.
 * Higher target frequency -> smaller divisor.
 * ======================================================================= */

static void test_divisor_100hz(void)
{
    /* 1193180 / 100 = 11931 (integer division) */
    ASSERT_EQ(pit_divisor(100), (uint16_t)11931u, "100 Hz divisor = 11931");
}

static void test_divisor_1000hz(void)
{
    /* 1193180 / 1000 = 1193 */
    ASSERT_EQ(pit_divisor(1000), (uint16_t)1193u, "1000 Hz divisor = 1193");
}

static void test_divisor_50hz(void)
{
    /* 1193180 / 50 = 23863 */
    ASSERT_EQ(pit_divisor(50), (uint16_t)23863u, "50 Hz divisor = 23863");
}

static void test_divisor_nonzero(void)
{
    ASSERT(pit_divisor(100)  != 0, "100 Hz divisor is nonzero");
    ASSERT(pit_divisor(250)  != 0, "250 Hz divisor is nonzero");
    ASSERT(pit_divisor(1000) != 0, "1000 Hz divisor is nonzero");
}

static void test_divisor_inversely_proportional(void)
{
    /* Higher frequency -> fewer cycles between IRQs -> smaller divisor. */
    ASSERT(pit_divisor(100) > pit_divisor(1000),
           "higher frequency produces a smaller reload divisor");
    ASSERT(pit_divisor(50) > pit_divisor(100),
           "50 Hz divisor > 100 Hz divisor");
}

/* =======================================================================
 * Scheduler -- initialisation
 * ======================================================================= */

static void test_init_task_count_zero(void)
{
    scheduler_initialize();
    ASSERT_EQ(scheduler_task_count(), 0u, "task count is 0 after init");
}

static void test_init_current_slot_unused(void)
{
    scheduler_initialize();
    task_t *t = scheduler_get_current();
    ASSERT_NOTNULL(t, "get_current returns non-NULL after init");
    ASSERT_EQ((int)t->state, (int)TASK_UNUSED,
              "current slot is UNUSED before any task is created");
}

/* =======================================================================
 * Scheduler -- task creation
 * ======================================================================= */

static void dummy_fn(void) {}

static void test_create_returns_valid_index(void)
{
    scheduler_initialize();
    int idx = scheduler_create_task(dummy_fn);
    ASSERT(idx >= 0,                    "create_task returns non-negative index");
    ASSERT(idx < SCHEDULER_MAX_TASKS,   "create_task index is within bounds");
}

static void test_create_increments_count(void)
{
    scheduler_initialize();
    scheduler_create_task(dummy_fn);
    ASSERT_EQ(scheduler_task_count(), 1u, "task count is 1 after one creation");
}

static void test_create_two_increments_count(void)
{
    scheduler_initialize();
    scheduler_create_task(dummy_fn);
    scheduler_create_task(dummy_fn);
    ASSERT_EQ(scheduler_task_count(), 2u, "task count is 2 after two creations");
}

static void test_create_multiple_unique_indices(void)
{
    scheduler_initialize();
    int a = scheduler_create_task(dummy_fn);
    int b = scheduler_create_task(dummy_fn);
    ASSERT(a >= 0, "first task index valid");
    ASSERT(b >= 0, "second task index valid");
    ASSERT(a != b, "two tasks receive distinct indices");
}

static void test_create_task_is_ready(void)
{
    scheduler_initialize();
    int idx = scheduler_create_task(dummy_fn);
    /* next_index only returns READY or RUNNING tasks; if it returns idx
     * then the created task must be in one of those states.            */
    int next = scheduler_next_index();
    ASSERT_EQ(next, idx,
              "newly created task is selected by next_index (state is READY)");
}

static void test_create_fills_all_slots(void)
{
    scheduler_initialize();
    for (int i = 0; i < SCHEDULER_MAX_TASKS; i++) {
        int r = scheduler_create_task(dummy_fn);
        ASSERT(r >= 0, "create succeeds while free slots remain");
    }
    ASSERT_EQ(scheduler_task_count(), (uint32_t)SCHEDULER_MAX_TASKS,
              "task count equals SCHEDULER_MAX_TASKS after filling all slots");
}

static void test_create_returns_minus1_when_full(void)
{
    scheduler_initialize();
    for (int i = 0; i < SCHEDULER_MAX_TASKS; i++)
        scheduler_create_task(dummy_fn);

    int extra = scheduler_create_task(dummy_fn);
    ASSERT_EQ(extra, -1, "create_task returns -1 when all slots are taken");
}

/* =======================================================================
 * Scheduler -- round-robin selection
 * ======================================================================= */

static void test_next_index_no_tasks(void)
{
    scheduler_initialize();
    /* No tasks; next_index must not crash and must return a valid index. */
    int next = scheduler_next_index();
    ASSERT(next >= 0,                  "next_index >= 0 with no tasks");
    ASSERT(next < SCHEDULER_MAX_TASKS, "next_index < MAX with no tasks");
}

static void test_next_index_finds_only_task(void)
{
    scheduler_initialize();
    int idx  = scheduler_create_task(dummy_fn);
    int next = scheduler_next_index();
    ASSERT_EQ(next, idx, "next_index returns the only READY task");
}

static void test_next_index_advances_past_current(void)
{
    scheduler_initialize();
    /* current_task = 0; create two tasks at indices 0 and 1. */
    int a = scheduler_create_task(dummy_fn);
    int b = scheduler_create_task(dummy_fn);
    (void)a;
    int next = scheduler_next_index();
    ASSERT_EQ(next, b,
              "next_index skips current slot and returns the next READY task");
}

/* =======================================================================
 * Scheduler -- tick and state transitions
 * ======================================================================= */

static void test_tick_switches_to_next_task(void)
{
    scheduler_initialize();
    int a = scheduler_create_task(dummy_fn);
    int b = scheduler_create_task(dummy_fn);
    (void)a;

    scheduler_tick();

    task_t *cur = scheduler_get_current();
    ASSERT_EQ((int)cur->state, (int)TASK_RUNNING,
              "current task is RUNNING after first tick");
    ASSERT_EQ((int)cur->id, b,
              "scheduler advanced to the second created task");
}

static void test_tick_sets_prev_running_to_ready(void)
{
    scheduler_initialize();
    scheduler_create_task(dummy_fn);
    scheduler_create_task(dummy_fn);

    /* Pretend slot 0 is actively running. */
    task_t *slot0 = scheduler_get_current();
    slot0->state = TASK_RUNNING;

    scheduler_tick();

    /* Slot 0 must have been demoted to READY. */
    ASSERT_EQ((int)slot0->state, (int)TASK_READY,
              "preempted task transitions from RUNNING to READY");
}

static void test_tick_round_robin_two_tasks(void)
{
    scheduler_initialize();
    int a = scheduler_create_task(dummy_fn);
    int b = scheduler_create_task(dummy_fn);
    (void)a;

    /* Tick 1: 0 -> b */
    scheduler_tick();
    int after_first = (int)scheduler_get_current()->id;

    /* Tick 2: b -> a (wraps around) */
    scheduler_tick();
    int after_second = (int)scheduler_get_current()->id;

    ASSERT(after_first != after_second,
           "round-robin alternates between two tasks on successive ticks");
    ASSERT_EQ(after_first,  b, "first tick advanced to task b");
    ASSERT_EQ(after_second, a, "second tick cycled back to task a");
}

static void test_tick_no_op_with_single_task(void)
{
    scheduler_initialize();
    scheduler_create_task(dummy_fn);

    /* Only one task: next_index returns current; tick must not change state. */
    task_t *before = scheduler_get_current();
    scheduler_tick();
    task_t *after  = scheduler_get_current();

    ASSERT(before == after, "tick is a no-op when only one task exists");
}

static void test_tick_no_op_with_no_tasks(void)
{
    scheduler_initialize();
    /* Must not crash with zero tasks. */
    scheduler_tick();
    ASSERT_EQ(scheduler_task_count(), 0u,
              "tick with no tasks leaves count at zero");
}

/* =======================================================================
 * main
 * ======================================================================= */

int main(void)
{
    /* PIT divisor math */
    RUN_SUITE(test_divisor_100hz);
    RUN_SUITE(test_divisor_1000hz);
    RUN_SUITE(test_divisor_50hz);
    RUN_SUITE(test_divisor_nonzero);
    RUN_SUITE(test_divisor_inversely_proportional);

    /* Scheduler init */
    RUN_SUITE(test_init_task_count_zero);
    RUN_SUITE(test_init_current_slot_unused);

    /* Task creation */
    RUN_SUITE(test_create_returns_valid_index);
    RUN_SUITE(test_create_increments_count);
    RUN_SUITE(test_create_two_increments_count);
    RUN_SUITE(test_create_multiple_unique_indices);
    RUN_SUITE(test_create_task_is_ready);
    RUN_SUITE(test_create_fills_all_slots);
    RUN_SUITE(test_create_returns_minus1_when_full);

    /* Round-robin selection */
    RUN_SUITE(test_next_index_no_tasks);
    RUN_SUITE(test_next_index_finds_only_task);
    RUN_SUITE(test_next_index_advances_past_current);

    /* Tick and state transitions */
    RUN_SUITE(test_tick_switches_to_next_task);
    RUN_SUITE(test_tick_sets_prev_running_to_ready);
    RUN_SUITE(test_tick_round_robin_two_tasks);
    RUN_SUITE(test_tick_no_op_with_single_task);
    RUN_SUITE(test_tick_no_op_with_no_tasks);

    TEST_SUMMARY();
}
