/*
 * Quilon OS — Wait Queue Unit Tests (section 12.2)
 *
 * Tests the linked-list mechanics of waitq_sleep, waitq_wake_one, and
 * waitq_wake_all on the host (no real scheduler).  scheduler_yield() is a
 * no-op in the host build, so waitq_sleep returns immediately after the
 * cleanup path — the entry is self-removed and the process state is
 * restored to PROC_RUNNING.
 *
 * Build & run:  cd tests && make
 */

#include "framework.h"

#include <stddef.h>
#include <stdint.h>

#include <kernel/waitq.h>
#include <kernel/process.h>

/* ── Helpers ──────────────────────────────────────────────────────────────── */

/* Manually push a pre-built entry onto the waitq (simulates a second sleeper
 * that called waitq_sleep on another CPU or in a previous turn).           */
static void push_entry(waitq_t *wq, waitq_entry_t *e, process_t *p)
{
    e->proc = p;
    e->next = wq->head;
    wq->head = e;
}

/* Statically-allocated processes so we don't blow the stack. */
static process_t tp[4];   /* test processes 0..3 */

static void reset_procs(void)
{
    for (int i = 0; i < 4; i++) {
        tp[i].state = PROC_UNUSED;
        tp[i].pid   = (uint32_t)i;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 1. WAITQ_INIT macro
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_init_head_null(void)
{
    waitq_t wq = WAITQ_INIT;
    ASSERT(wq.head == NULL, "WAITQ_INIT: head is NULL");
}

static void test_two_inits_independent(void)
{
    waitq_t a = WAITQ_INIT;
    waitq_t b = WAITQ_INIT;
    ASSERT(a.head == NULL, "WAITQ_INIT a: head NULL");
    ASSERT(b.head == NULL, "WAITQ_INIT b: head NULL");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 2. waitq_wake_one
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_wake_one_empty_nop(void)
{
    waitq_t wq = WAITQ_INIT;
    waitq_wake_one(&wq);   /* must not crash */
    ASSERT(wq.head == NULL, "wake_one on empty: head stays NULL");
}

static void test_wake_one_single_entry(void)
{
    reset_procs();
    waitq_t wq = WAITQ_INIT;
    tp[0].state = PROC_BLOCKED;
    waitq_entry_t e;
    push_entry(&wq, &e, &tp[0]);

    waitq_wake_one(&wq);

    ASSERT_EQ((int)tp[0].state, (int)PROC_READY, "wake_one: process → PROC_READY");
    ASSERT(wq.head == NULL, "wake_one: queue empty after sole entry");
}

static void test_wake_one_lifo_order(void)
{
    reset_procs();
    waitq_t wq = WAITQ_INIT;
    tp[0].state = PROC_BLOCKED;
    tp[1].state = PROC_BLOCKED;
    waitq_entry_t e0, e1;
    push_entry(&wq, &e0, &tp[0]);   /* e0 pushed first */
    push_entry(&wq, &e1, &tp[1]);   /* e1 is new head (LIFO) */

    waitq_wake_one(&wq);   /* wakes head = tp[1] */

    ASSERT_EQ((int)tp[1].state, (int)PROC_READY,   "wake_one: head (tp[1]) woken first");
    ASSERT_EQ((int)tp[0].state, (int)PROC_BLOCKED, "wake_one: tail (tp[0]) still blocked");
    ASSERT(wq.head == &e0, "wake_one: e0 is new head");
}

static void test_wake_one_drains_two(void)
{
    reset_procs();
    waitq_t wq = WAITQ_INIT;
    tp[0].state = PROC_BLOCKED;
    tp[1].state = PROC_BLOCKED;
    waitq_entry_t e0, e1;
    push_entry(&wq, &e0, &tp[0]);
    push_entry(&wq, &e1, &tp[1]);

    waitq_wake_one(&wq);
    waitq_wake_one(&wq);

    ASSERT_EQ((int)tp[0].state, (int)PROC_READY, "wake_one×2: tp[0] woken");
    ASSERT_EQ((int)tp[1].state, (int)PROC_READY, "wake_one×2: tp[1] woken");
    ASSERT(wq.head == NULL, "wake_one×2: queue empty");
}

static void test_wake_one_extra_call_on_empty(void)
{
    reset_procs();
    waitq_t wq = WAITQ_INIT;
    tp[0].state = PROC_BLOCKED;
    waitq_entry_t e;
    push_entry(&wq, &e, &tp[0]);
    waitq_wake_one(&wq);
    waitq_wake_one(&wq);   /* second call on now-empty queue: no-op */
    ASSERT(wq.head == NULL, "wake_one on already-empty: still NULL");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 3. waitq_wake_all
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_wake_all_empty_nop(void)
{
    waitq_t wq = WAITQ_INIT;
    waitq_wake_all(&wq);
    ASSERT(wq.head == NULL, "wake_all on empty: head stays NULL");
}

static void test_wake_all_single(void)
{
    reset_procs();
    waitq_t wq = WAITQ_INIT;
    tp[0].state = PROC_BLOCKED;
    waitq_entry_t e;
    push_entry(&wq, &e, &tp[0]);

    waitq_wake_all(&wq);

    ASSERT_EQ((int)tp[0].state, (int)PROC_READY, "wake_all single: woken");
    ASSERT(wq.head == NULL, "wake_all single: queue empty");
}

static void test_wake_all_three(void)
{
    reset_procs();
    waitq_t wq = WAITQ_INIT;
    tp[0].state = PROC_BLOCKED;
    tp[1].state = PROC_BLOCKED;
    tp[2].state = PROC_BLOCKED;
    waitq_entry_t e0, e1, e2;
    push_entry(&wq, &e0, &tp[0]);
    push_entry(&wq, &e1, &tp[1]);
    push_entry(&wq, &e2, &tp[2]);

    waitq_wake_all(&wq);

    ASSERT_EQ((int)tp[0].state, (int)PROC_READY, "wake_all: tp[0] woken");
    ASSERT_EQ((int)tp[1].state, (int)PROC_READY, "wake_all: tp[1] woken");
    ASSERT_EQ((int)tp[2].state, (int)PROC_READY, "wake_all: tp[2] woken");
    ASSERT(wq.head == NULL, "wake_all three: queue empty");
}

static void test_wake_all_idempotent(void)
{
    reset_procs();
    waitq_t wq = WAITQ_INIT;
    tp[0].state = PROC_BLOCKED;
    waitq_entry_t e;
    push_entry(&wq, &e, &tp[0]);
    waitq_wake_all(&wq);
    waitq_wake_all(&wq);   /* second call on empty queue */
    ASSERT(wq.head == NULL, "wake_all idempotent: still empty");
}

static void test_wake_all_detaches_whole_list(void)
{
    reset_procs();
    waitq_t wq = WAITQ_INIT;
    for (int i = 0; i < 4; i++) {
        tp[i].state = PROC_BLOCKED;
    }
    waitq_entry_t e[4];
    for (int i = 0; i < 4; i++) push_entry(&wq, &e[i], &tp[i]);

    waitq_wake_all(&wq);

    for (int i = 0; i < 4; i++) {
        ASSERT_EQ((int)tp[i].state, (int)PROC_READY, "wake_all 4: each woken");
    }
    ASSERT(wq.head == NULL, "wake_all 4: list fully detached");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 4. waitq_sleep (no-op scheduler in host build)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_sleep_null_process_nop(void)
{
    /* waitq_sleep with current_process == NULL must return without
     * touching the queue or crashing.                               */
    waitq_t wq = WAITQ_INIT;
    process_t *saved = current_process;
    current_process = NULL;
    waitq_sleep(&wq);
    current_process = saved;
    ASSERT(wq.head == NULL, "sleep NULL current_process: queue untouched");
}

static void test_sleep_self_removes_entry(void)
{
    /* In the host build scheduler_yield is a no-op, so sleep returns
     * immediately.  The entry must be self-removed from the list.   */
    reset_procs();
    waitq_t wq = WAITQ_INIT;
    tp[0].state    = PROC_RUNNING;
    current_process = &tp[0];

    waitq_sleep(&wq);

    ASSERT(wq.head == NULL, "sleep: entry self-removed on return");
    current_process = NULL;
}

static void test_sleep_restores_running_state(void)
{
    reset_procs();
    waitq_t wq = WAITQ_INIT;
    tp[0].state    = PROC_RUNNING;
    current_process = &tp[0];

    waitq_sleep(&wq);

    ASSERT_EQ((int)tp[0].state, (int)PROC_RUNNING,
              "sleep: state restored to PROC_RUNNING on return");
    current_process = NULL;
}

static void test_sleep_twice_no_leak(void)
{
    /* Calling sleep twice in a row must not leave stale entries. */
    reset_procs();
    waitq_t wq = WAITQ_INIT;
    tp[0].state    = PROC_RUNNING;
    current_process = &tp[0];

    waitq_sleep(&wq);
    waitq_sleep(&wq);

    ASSERT(wq.head == NULL, "sleep×2: no stale entries");
    ASSERT_EQ((int)tp[0].state, (int)PROC_RUNNING, "sleep×2: state still RUNNING");
    current_process = NULL;
}

static void test_sleep_leaves_other_entries_intact(void)
{
    /* A second sleeper (tp[1]) is already in the queue.  tp[0] calls
     * sleep, which adds its entry on top and then self-removes it on
     * return.  tp[1]'s entry should still be there.                 */
    reset_procs();
    waitq_t wq = WAITQ_INIT;
    tp[0].state = PROC_RUNNING;
    tp[1].state = PROC_BLOCKED;

    waitq_entry_t e1 = { .proc = &tp[1], .next = NULL };
    wq.head = &e1;

    current_process = &tp[0];
    waitq_sleep(&wq);

    ASSERT(wq.head == &e1,              "sleep: tp[1] entry still in queue");
    ASSERT_EQ((int)tp[1].state, (int)PROC_BLOCKED, "sleep: tp[1] still blocked");
    ASSERT_EQ((int)tp[0].state, (int)PROC_RUNNING, "sleep: tp[0] back to running");
    current_process = NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 5. Combined sleep + wake
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_sleep_then_wake_one(void)
{
    /* tp[1] is "already sleeping" (entry pre-pushed); tp[0] also sleeps
     * (self-removes on return since yield is no-op); then wake_one wakes
     * tp[1].                                                             */
    reset_procs();
    waitq_t wq = WAITQ_INIT;
    tp[0].state = PROC_RUNNING;
    tp[1].state = PROC_BLOCKED;

    waitq_entry_t e1 = { .proc = &tp[1], .next = NULL };
    wq.head = &e1;

    current_process = &tp[0];
    waitq_sleep(&wq);   /* tp[0] self-removes; tp[1] entry remains */

    ASSERT(wq.head == &e1, "before wake_one: tp[1] still queued");

    waitq_wake_one(&wq);
    ASSERT_EQ((int)tp[1].state, (int)PROC_READY, "after wake_one: tp[1] ready");
    ASSERT(wq.head == NULL, "after wake_one: queue empty");
    current_process = NULL;
}

static void test_sleep_then_wake_all(void)
{
    reset_procs();
    waitq_t wq = WAITQ_INIT;
    tp[0].state = PROC_RUNNING;
    tp[1].state = PROC_BLOCKED;
    tp[2].state = PROC_BLOCKED;

    waitq_entry_t e1 = { .proc = &tp[1], .next = NULL };
    waitq_entry_t e2 = { .proc = &tp[2], .next = &e1 };
    wq.head = &e2;

    current_process = &tp[0];
    waitq_sleep(&wq);

    waitq_wake_all(&wq);

    ASSERT_EQ((int)tp[1].state, (int)PROC_READY, "wake_all: tp[1] ready");
    ASSERT_EQ((int)tp[2].state, (int)PROC_READY, "wake_all: tp[2] ready");
    ASSERT(wq.head == NULL, "wake_all: queue empty");
    current_process = NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 6. Partial wake (wake_one N times, not all)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_partial_wake(void)
{
    reset_procs();
    waitq_t wq = WAITQ_INIT;
    tp[0].state = PROC_BLOCKED;
    tp[1].state = PROC_BLOCKED;
    tp[2].state = PROC_BLOCKED;
    waitq_entry_t e0, e1, e2;
    push_entry(&wq, &e0, &tp[0]);
    push_entry(&wq, &e1, &tp[1]);
    push_entry(&wq, &e2, &tp[2]);

    waitq_wake_one(&wq);   /* wakes tp[2] (LIFO head) */
    waitq_wake_one(&wq);   /* wakes tp[1] */

    int ready   = 0;
    int blocked = 0;
    for (int i = 0; i < 3; i++) {
        if (tp[i].state == PROC_READY)   ready++;
        else if (tp[i].state == PROC_BLOCKED) blocked++;
    }
    ASSERT_EQ(ready,   2, "partial wake: exactly 2 processes ready");
    ASSERT_EQ(blocked, 1, "partial wake: exactly 1 process still blocked");
    ASSERT(wq.head != NULL, "partial wake: queue not yet empty");
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main(void)
{
    current_process = NULL;

    RUN_SUITE(test_init_head_null);
    RUN_SUITE(test_two_inits_independent);

    RUN_SUITE(test_wake_one_empty_nop);
    RUN_SUITE(test_wake_one_single_entry);
    RUN_SUITE(test_wake_one_lifo_order);
    RUN_SUITE(test_wake_one_drains_two);
    RUN_SUITE(test_wake_one_extra_call_on_empty);

    RUN_SUITE(test_wake_all_empty_nop);
    RUN_SUITE(test_wake_all_single);
    RUN_SUITE(test_wake_all_three);
    RUN_SUITE(test_wake_all_idempotent);
    RUN_SUITE(test_wake_all_detaches_whole_list);

    RUN_SUITE(test_sleep_null_process_nop);
    RUN_SUITE(test_sleep_self_removes_entry);
    RUN_SUITE(test_sleep_restores_running_state);
    RUN_SUITE(test_sleep_twice_no_leak);
    RUN_SUITE(test_sleep_leaves_other_entries_intact);

    RUN_SUITE(test_sleep_then_wake_one);
    RUN_SUITE(test_sleep_then_wake_all);

    RUN_SUITE(test_partial_wake);

    TEST_SUMMARY();
}
