/*
 * Quilon OS -- Kernel Thread (clone) Unit Tests (section 12.3)
 *
 * Host-side tests: compiled with native gcc, no QEMU needed.
 *
 * What is tested (pure C, no x86 assembly):
 *   • SYS_CLONE / CLONE_* constant values
 *   • process_t.thread_group initialisation (0 after process_init)
 *   • process_create() sets thread_group = 0
 *   • thread_group can be written and read back
 *   • thread_group field offset is within process_t (no layout regression)
 *   • CLONE_VM, CLONE_FS, CLONE_FILES are distinct and non-overlapping
 *   • SYS_CLONE = 32 (one past SYS_RENAME = 31)
 *
 * What is NOT tested (requires x86 / QEMU):
 *   • The iret frame built by SYS_CLONE -- only safe on real x86
 *   • The actual execution of a thread function
 *   • Shared address-space semantics (require a real page directory)
 *
 * Build & run:  cd tests && make
 */

#include "framework.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/process.h>
#include <kernel/syscall.h>

/* ===========================================================================
 * 1. SYS_CLONE and CLONE_* constant values
 * =========================================================================== */

static void test_syscall_clone_number(void)
{
    ASSERT_EQ(SYS_CLONE, 32u, "SYS_CLONE == 32");
    /* Must be one past SYS_RENAME */
    ASSERT_EQ(SYS_CLONE, (uint32_t)(SYS_RENAME + 1), "SYS_CLONE == SYS_RENAME + 1");
}

static void test_clone_flags_distinct(void)
{
    /* Each CLONE_* flag occupies a unique bit. */
    ASSERT(CLONE_VM    != CLONE_FS,    "CLONE_VM   != CLONE_FS");
    ASSERT(CLONE_VM    != CLONE_FILES, "CLONE_VM   != CLONE_FILES");
    ASSERT(CLONE_FS    != CLONE_FILES, "CLONE_FS   != CLONE_FILES");
}

static void test_clone_flags_no_overlap(void)
{
    /* No two flags share any bit. */
    ASSERT((CLONE_VM    & CLONE_FS)    == 0u, "CLONE_VM    & CLONE_FS    == 0");
    ASSERT((CLONE_VM    & CLONE_FILES) == 0u, "CLONE_VM    & CLONE_FILES == 0");
    ASSERT((CLONE_FS    & CLONE_FILES) == 0u, "CLONE_FS    & CLONE_FILES == 0");
}

static void test_clone_flags_values(void)
{
    ASSERT_EQ(CLONE_VM,    0x0100u, "CLONE_VM    == 0x0100");
    ASSERT_EQ(CLONE_FS,    0x0200u, "CLONE_FS    == 0x0200");
    ASSERT_EQ(CLONE_FILES, 0x0400u, "CLONE_FILES == 0x0400");
}

/* ===========================================================================
 * 2. thread_group field in process_t
 * =========================================================================== */

static void test_thread_group_init_zero(void)
{
    process_init();

    for (int i = 0; i < PROCESS_MAX; i++) {
        ASSERT_EQ(process_table[i].thread_group, 0u,
                  "process_init: thread_group == 0 for all slots");
    }
}

static void test_thread_group_create_zero(void)
{
    process_init();
    process_t *p = process_create("tg-test", 0x1000, 0x2000);

    ASSERT(p != NULL, "process_create: non-NULL");
    ASSERT_EQ(p->thread_group, 0u,
              "process_create: thread_group initialised to 0");
}

static void test_thread_group_settable(void)
{
    process_init();
    process_t *leader = process_create("leader", 0x1000, 0x1000);
    process_t *thread = process_create("worker", 0x1000, 0x1000);

    ASSERT(leader != NULL && thread != NULL, "create leader and thread: non-NULL");

    /* Simulate what SYS_CLONE does: mark thread as belonging to leader's group. */
    thread->thread_group = leader->pid;

    ASSERT_EQ(thread->thread_group, leader->pid,
              "thread_group can be set to leader's pid");
    ASSERT_EQ(leader->thread_group, 0u,
              "leader's thread_group remains 0");
}

static void test_thread_group_leader_detection(void)
{
    process_init();
    process_t *p = process_create("proc", 0, 0);
    ASSERT(p != NULL, "create: non-NULL");

    /* thread_group == 0 means process leader. */
    ASSERT(p->thread_group == 0u, "new process is a group leader (thread_group==0)");

    /* Set a non-zero thread_group to simulate a thread. */
    p->thread_group = 99u;
    ASSERT(p->thread_group != 0u, "thread has non-zero thread_group");
}

static void test_thread_group_field_within_pcb(void)
{
    process_t dummy;
    /* thread_group must be accessible as a uint32_t -- no trap on access. */
    dummy.thread_group = 0xDEADBEEFu;
    ASSERT_EQ(dummy.thread_group, 0xDEADBEEFu,
              "thread_group field is accessible and stores a full uint32_t");
}

/* ===========================================================================
 * 3. thread_group propagation for nested threads
 * =========================================================================== */

static void test_thread_group_chain(void)
{
    process_init();

    /* Consume PID 0 so the leader gets a non-zero PID; thread_group==0 means
     * "group leader", so a leader with pid==0 would be indistinguishable from
     * "no group" in the ternary chain logic below. */
    process_create("idle", 0, 0);

    process_t *leader  = process_create("leader", 0, 0);
    process_t *thread1 = process_create("t1",     0, 0);
    process_t *thread2 = process_create("t2",     0, 0);

    ASSERT(leader && thread1 && thread2, "three creates succeed");

    /* thread1 belongs to leader's group. */
    thread1->thread_group = leader->pid;

    /* thread2 created from thread1: still belongs to the same group (leader). */
    uint32_t group = thread1->thread_group ? thread1->thread_group : thread1->pid;
    thread2->thread_group = group;

    ASSERT_EQ(thread2->thread_group, leader->pid,
              "nested thread inherits original group leader's pid");
}

/* ===========================================================================
 * 4. CLONE_VM combinations and bit arithmetic
 * =========================================================================== */

static void test_clone_thread_flags_combined(void)
{
    uint32_t thread_flags = CLONE_VM | CLONE_FS | CLONE_FILES;

    ASSERT((thread_flags & CLONE_VM)    != 0u, "combined: CLONE_VM set");
    ASSERT((thread_flags & CLONE_FS)    != 0u, "combined: CLONE_FS set");
    ASSERT((thread_flags & CLONE_FILES) != 0u, "combined: CLONE_FILES set");
    ASSERT_EQ(thread_flags, 0x0700u, "combined: CLONE_VM|FS|FILES == 0x0700");
}

static void test_clone_vm_flag_check(void)
{
    /* Process (no CLONE_VM): new address space. */
    uint32_t proc_flags = 0u;
    ASSERT((proc_flags & CLONE_VM) == 0u, "proc flags: no CLONE_VM");

    /* Thread (CLONE_VM): shared address space. */
    uint32_t thread_flags = CLONE_VM | CLONE_FS | CLONE_FILES;
    ASSERT((thread_flags & CLONE_VM) != 0u, "thread flags: CLONE_VM set");
}

/* ===========================================================================
 * main
 * =========================================================================== */

int main(void)
{
    /* SYS_CLONE constant */
    RUN_SUITE(test_syscall_clone_number);

    /* CLONE_* flags */
    RUN_SUITE(test_clone_flags_distinct);
    RUN_SUITE(test_clone_flags_no_overlap);
    RUN_SUITE(test_clone_flags_values);

    /* thread_group field */
    RUN_SUITE(test_thread_group_init_zero);
    RUN_SUITE(test_thread_group_create_zero);
    RUN_SUITE(test_thread_group_settable);
    RUN_SUITE(test_thread_group_leader_detection);
    RUN_SUITE(test_thread_group_field_within_pcb);

    /* thread_group propagation */
    RUN_SUITE(test_thread_group_chain);

    /* CLONE_VM flag arithmetic */
    RUN_SUITE(test_clone_thread_flags_combined);
    RUN_SUITE(test_clone_vm_flag_check);

    TEST_SUMMARY();
}
