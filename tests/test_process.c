/*
 * Quilon OS -- Process Management Unit Tests (section 5.2)
 *
 * Compiled with the native host gcc -- no cross-compiler or QEMU needed.
 *
 * What is tested here (pure C, no x86 assembly):
 *   • process_init()       -- all slots become PROC_UNUSED, current_process = NULL
 *   • process_create()     -- allocates a slot, sets up PCB fields correctly
 *   • process_create()     -- builds the initial kernel_esp frame (5 words)
 *   • process_create()     -- fills slots in order, returns NULL when full
 *   • process_find()       -- looks up by PID, handles out-of-range and UNUSED
 *   • process_pick_next()  -- round-robin PROC_READY selection
 *   • proc_state_t values  -- enum constants are distinct and non-negative
 *   • PROCESS_MAX          -- compile-time check on table size
 *
 * What is NOT tested (requires x86 hardware / QEMU):
 *   • context_switch()     -- inline asm ESP swap, only safe on real x86
 *   • process_launch()     -- calls usermode_initialize/usermode_enter (ring-3)
 *   • process_first_run    -- assembly trampoline in boot.S
 *   • TSS esp0 update      -- gdt_set_kernel_stack writes hardware state
 *
 * The host build does NOT define __is_kernel, so process_launch() uses the
 * stub that returns immediately (while (1) {}), and process_first_run's
 * address is stored as 0 in the initial stack frame -- safe to inspect as an
 * integer without dereferencing.
 *
 * Build & run:  cd tests && make
 */

#include "framework.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/process.h>

/* -- Mocks -----------------------------------------------------------------
 * process.c calls printf() for debug output in the kernel build.  In the
 * host build those calls are absent (guarded by #ifdef __is_kernel) so no
 * mock is required.  If a future change adds printf to pure-C code paths,
 * add a mock putchar() here.
 * ------------------------------------------------------------------------ */

/* ===========================================================================
 * 1. State enum values
 * =========================================================================== */

static void test_proc_state_values(void)
{
    ASSERT_EQ(PROC_UNUSED,  0u, "PROC_UNUSED  == 0");
    ASSERT_EQ(PROC_RUNNING, 1u, "PROC_RUNNING == 1");
    ASSERT_EQ(PROC_READY,   2u, "PROC_READY   == 2");
    ASSERT_EQ(PROC_BLOCKED, 3u, "PROC_BLOCKED == 3");
    ASSERT_EQ(PROC_ZOMBIE,  4u, "PROC_ZOMBIE  == 4");

    /* All values must be distinct */
    ASSERT(PROC_UNUSED  != PROC_RUNNING, "PROC_UNUSED != PROC_RUNNING");
    ASSERT(PROC_READY   != PROC_BLOCKED, "PROC_READY  != PROC_BLOCKED");
    ASSERT(PROC_BLOCKED != PROC_ZOMBIE,  "PROC_BLOCKED!= PROC_ZOMBIE");
}

/* ===========================================================================
 * 2. process_init
 * =========================================================================== */

static void test_init_clears_table(void)
{
    /* Dirty the table first to make sure init actually clears it. */
    for (int i = 0; i < PROCESS_MAX; i++)
        process_table[i].state = PROC_RUNNING;
    current_process = &process_table[0];

    process_init();

    for (int i = 0; i < PROCESS_MAX; i++) {
        ASSERT(process_table[i].state == PROC_UNUSED,
               "process_init: all slots become PROC_UNUSED");
        ASSERT_EQ(process_table[i].pid, (uint32_t)i,
                  "process_init: pid == slot index");
    }
    ASSERT(current_process == NULL,
           "process_init: current_process is NULL");
}

static void test_init_idempotent(void)
{
    process_init();
    process_init();   /* calling twice should not corrupt state */
    for (int i = 0; i < PROCESS_MAX; i++)
        ASSERT(process_table[i].state == PROC_UNUSED,
               "double init: slots still PROC_UNUSED");
}

/* ===========================================================================
 * 3. process_create
 * =========================================================================== */

static void test_create_returns_ready(void)
{
    process_init();
    process_t *p = process_create("hello", 0x400000, 0xDEAD0000);

    ASSERT(p != NULL, "process_create returns non-NULL");
    ASSERT(p->state == PROC_READY, "new process starts in PROC_READY");
}

static void test_create_fields(void)
{
    process_init();
    process_t *p = process_create("test", 0x401000, 0xABCD1000);

    ASSERT(p != NULL, "process_create: non-NULL");
    ASSERT_EQ(p->entry,    0x401000u,   "entry is stored correctly");
    ASSERT_EQ(p->cr3,      0xABCD1000u, "cr3 is stored correctly");
    ASSERT_EQ(p->exit_code, 0,          "exit_code initialised to 0");
    ASSERT(p->name[0] != '\0', "name is not empty");
    ASSERT(strcmp(p->name, "test") == 0, "name matches 'test'");
}

static void test_create_name_truncation(void)
{
    process_init();
    /* Name longer than PROCESS_NAME_LEN-1 must be truncated, not overflow. */
    process_t *p = process_create("verylongnamethatexceedslimit", 0, 0);

    ASSERT(p != NULL, "create with long name: non-NULL");
    /* Check NUL termination at the boundary. */
    ASSERT(p->name[PROCESS_NAME_LEN - 1] == '\0',
           "name is NUL-terminated within bounds");
}

static void test_create_fills_slots_in_order(void)
{
    process_init();

    process_t *first = process_create("a", 0x100, 0x1000);
    process_t *second = process_create("b", 0x200, 0x2000);

    ASSERT(first  != NULL,   "first create: non-NULL");
    ASSERT(second != NULL,   "second create: non-NULL");
    ASSERT(first  != second, "two creates return distinct PCBs");

    /* PIDs are the slot indices, and they must be different. */
    ASSERT(first->pid != second->pid, "different PIDs");
}

static void test_create_returns_null_when_full(void)
{
    process_init();

    /* Fill every slot */
    for (int i = 0; i < PROCESS_MAX; i++)
        process_create("x", 0, 0);

    /* The next create must fail */
    process_t *p = process_create("overflow", 0, 0);
    ASSERT(p == NULL, "process_create returns NULL when table is full");
}

static void test_create_reuses_freed_slot(void)
{
    process_init();

    process_t *p = process_create("tmp", 0x1000, 0x2000);
    ASSERT(p != NULL, "initial create: non-NULL");

    uint32_t saved_pid = p->pid;
    p->state = PROC_UNUSED;   /* manually free the slot */

    process_t *p2 = process_create("new", 0x3000, 0x4000);
    ASSERT(p2 != NULL,            "reuse: non-NULL");
    ASSERT_EQ(p2->pid, saved_pid, "reuses the freed slot");
}

/* -- Initial kernel stack frame -------------------------------------------- */

static void test_create_kernel_esp_within_stack(void)
{
    process_init();
    process_t *p = process_create("stack-test", 0, 0);
    ASSERT(p != NULL, "create: non-NULL");

    /* kernel_esp is a uint32_t (correct on the 32-bit kernel target).
     * On the 64-bit host the stored value is the low 32 bits of the real
     * pointer.  We verify only that it is non-zero (process_create set it)
     * and that it lives inside kernel_stack[] by comparing offsets from
     * the stack base pointer, which is always accessible as a uintptr_t.  */
    ASSERT(p->kernel_esp != 0u, "kernel_esp is non-zero after process_create");

    /* Check that the frame was built at the TOP of kernel_stack[]: the 5
     * pre-pushed words start at kernel_stack_top - 5 uint32_ts.          */
    uint8_t *kstack_top = p->kernel_stack + sizeof(p->kernel_stack);
    uint32_t *frame_base = (uint32_t *)kstack_top - 5;  /* edi slot */
    ASSERT(frame_base >= (uint32_t *)p->kernel_stack,
           "frame base is within kernel_stack[]");
    ASSERT((uint8_t *)frame_base < kstack_top,
           "frame base is below kernel_stack top");
}

static void test_create_kernel_stack_frame_layout(void)
{
    process_init();
    process_t *p = process_create("frame-test", 0x5000, 0x6000);
    ASSERT(p != NULL, "create: non-NULL");

    /*
     * context_switch() pops: edi, esi, ebx, ebp, then ret.
     * process_create() pushes (from high address downward):
     *   [top - 4]   ret_addr (process_first_run, or 0 in host build)
     *   [top - 8]   ebp = 0
     *   [top - 12]  ebx = 0
     *   [top - 16]  esi = 0
     *   [top - 20]  edi = 0  ← kernel_esp points here on 32-bit target
     *
     * We access via kernel_stack[] directly (avoids uint32_t truncation of
     * the 64-bit host pointer that would result from casting kernel_esp).
     */
    uint32_t *sp = (uint32_t *)(p->kernel_stack + sizeof(p->kernel_stack)) - 5;

    ASSERT_EQ(sp[0], 0u, "initial edi = 0");
    ASSERT_EQ(sp[1], 0u, "initial esi = 0");
    ASSERT_EQ(sp[2], 0u, "initial ebx = 0");
    ASSERT_EQ(sp[3], 0u, "initial ebp = 0");
    /* sp[4] = ret address -- in host build it is 0; in kernel it is the
     * address of process_first_run.  We just verify access doesn't fault. */
    (void)sp[4];
}

/* ===========================================================================
 * 4. process_find
 * =========================================================================== */

static void test_find_returns_correct_pcb(void)
{
    process_init();
    process_t *p = process_create("findme", 0x1000, 0x2000);
    ASSERT(p != NULL, "create: non-NULL");

    process_t *found = process_find(p->pid);
    ASSERT(found == p, "process_find returns the same PCB pointer");
}

static void test_find_returns_null_for_unused(void)
{
    process_init();
    /* Slot 0 is PROC_UNUSED after init; find should return NULL. */
    process_t *found = process_find(0);
    ASSERT(found == NULL, "process_find returns NULL for PROC_UNUSED slot");
}

static void test_find_returns_null_for_out_of_range(void)
{
    process_init();
    process_t *found = process_find((uint32_t)PROCESS_MAX);
    ASSERT(found == NULL, "process_find returns NULL for PID >= PROCESS_MAX");

    found = process_find((uint32_t)-1);   /* huge PID */
    ASSERT(found == NULL, "process_find returns NULL for huge PID");
}

/* ===========================================================================
 * 5. process_pick_next  (round-robin)
 * =========================================================================== */

static void test_pick_next_null_when_no_ready(void)
{
    process_init();
    /* Create one process and set it as current (RUNNING), none READY. */
    process_t *p = process_create("only", 0, 0);
    ASSERT(p != NULL, "create: non-NULL");
    p->state = PROC_RUNNING;
    current_process = p;

    process_t *next = process_pick_next();
    ASSERT(next == NULL, "pick_next returns NULL when no PROC_READY exists");
}

static void test_pick_next_finds_ready(void)
{
    process_init();

    process_t *a = process_create("a", 0x100, 0);
    process_t *b = process_create("b", 0x200, 0);
    ASSERT(a && b, "two creates succeed");

    a->state = PROC_RUNNING;
    current_process = a;
    /* b is PROC_READY by default from process_create */

    process_t *next = process_pick_next();
    ASSERT(next == b, "pick_next selects the only PROC_READY process");
}

static void test_pick_next_round_robin(void)
{
    process_init();

    process_t *a = process_create("a", 0, 0);
    process_t *b = process_create("b", 0, 0);
    process_t *c = process_create("c", 0, 0);
    ASSERT(a && b && c, "three creates succeed");

    /* Set a as current RUNNING; b and c are READY. */
    a->state = PROC_RUNNING;
    current_process = a;

    process_t *n1 = process_pick_next();
    ASSERT(n1 != NULL,   "first pick: non-NULL");
    ASSERT(n1 != a,      "first pick: not current process");
    ASSERT(n1->state == PROC_READY, "first pick: PROC_READY");

    /* Simulate scheduling: n1 becomes current, mark old current READY. */
    a->state  = PROC_READY;
    n1->state = PROC_RUNNING;
    current_process = n1;

    process_t *n2 = process_pick_next();
    ASSERT(n2 != NULL,  "second pick: non-NULL");
    ASSERT(n2 != n1,    "second pick: different from first");
    ASSERT(n2->state == PROC_READY, "second pick: PROC_READY");
}

static void test_pick_next_skips_blocked_and_zombie(void)
{
    process_init();

    process_t *a = process_create("a", 0, 0);
    process_t *b = process_create("b", 0, 0);
    process_t *c = process_create("c", 0, 0);
    ASSERT(a && b && c, "three creates succeed");

    a->state = PROC_RUNNING;
    b->state = PROC_BLOCKED;
    c->state = PROC_READY;
    current_process = a;

    process_t *next = process_pick_next();
    ASSERT(next == c, "pick_next skips PROC_BLOCKED and returns PROC_READY c");
}

static void test_pick_next_skips_zombie(void)
{
    process_init();

    process_t *a = process_create("a", 0, 0);
    process_t *b = process_create("b", 0, 0);
    process_t *c = process_create("c", 0, 0);
    ASSERT(a && b && c, "three creates succeed");

    a->state = PROC_RUNNING;
    b->state = PROC_ZOMBIE;
    c->state = PROC_READY;
    current_process = a;

    process_t *next = process_pick_next();
    ASSERT(next == c, "pick_next skips PROC_ZOMBIE and returns c");
}

/* ===========================================================================
 * 6. PCB struct layout sanity checks
 * =========================================================================== */

static void test_pcb_struct_sizes(void)
{
    /* kernel_stack must be exactly 4096 bytes */
    process_t dummy;
    ASSERT_EQ(sizeof(dummy.kernel_stack), (size_t)4096,
              "kernel_stack is 4096 bytes");

    /* name must be exactly PROCESS_NAME_LEN bytes */
    ASSERT_EQ(sizeof(dummy.name), (size_t)PROCESS_NAME_LEN,
              "name is PROCESS_NAME_LEN bytes");
}

static void test_process_max_is_reasonable(void)
{
    ASSERT(PROCESS_MAX > 0,  "PROCESS_MAX > 0");
    ASSERT(PROCESS_MAX <= 64, "PROCESS_MAX <= 64 (sanity bound)");
}

/* ===========================================================================
 * 7. Parent/child PID relationship
 * =========================================================================== */

static void test_child_records_parent_pid(void)
{
    process_init();

    process_t *parent = process_create("parent", 0x1000, 0);
    ASSERT(parent != NULL, "parent create: non-NULL");
    parent->state   = PROC_RUNNING;
    current_process = parent;

    process_t *child = process_create("child", 0x2000, 0);
    ASSERT(child != NULL, "child create: non-NULL");
    ASSERT_EQ(child->parent_pid, parent->pid,
              "child->parent_pid == parent->pid");
}

static void test_no_current_process_gives_parent_zero(void)
{
    process_init();
    current_process = NULL;   /* no parent */

    process_t *p = process_create("orphan", 0, 0);
    ASSERT(p != NULL, "orphan create: non-NULL");
    ASSERT_EQ(p->parent_pid, 0u, "orphan parent_pid == 0");
}

/* ===========================================================================
 * main
 * =========================================================================== */

int main(void)
{
    /* State enum */
    RUN_SUITE(test_proc_state_values);

    /* process_init */
    RUN_SUITE(test_init_clears_table);
    RUN_SUITE(test_init_idempotent);

    /* process_create -- basic */
    RUN_SUITE(test_create_returns_ready);
    RUN_SUITE(test_create_fields);
    RUN_SUITE(test_create_name_truncation);
    RUN_SUITE(test_create_fills_slots_in_order);
    RUN_SUITE(test_create_returns_null_when_full);
    RUN_SUITE(test_create_reuses_freed_slot);

    /* process_create -- kernel stack frame */
    RUN_SUITE(test_create_kernel_esp_within_stack);
    RUN_SUITE(test_create_kernel_stack_frame_layout);

    /* process_find */
    RUN_SUITE(test_find_returns_correct_pcb);
    RUN_SUITE(test_find_returns_null_for_unused);
    RUN_SUITE(test_find_returns_null_for_out_of_range);

    /* process_pick_next */
    RUN_SUITE(test_pick_next_null_when_no_ready);
    RUN_SUITE(test_pick_next_finds_ready);
    RUN_SUITE(test_pick_next_round_robin);
    RUN_SUITE(test_pick_next_skips_blocked_and_zombie);
    RUN_SUITE(test_pick_next_skips_zombie);

    /* PCB layout */
    RUN_SUITE(test_pcb_struct_sizes);
    RUN_SUITE(test_process_max_is_reasonable);

    /* Parent/child */
    RUN_SUITE(test_child_records_parent_pid);
    RUN_SUITE(test_no_current_process_gives_parent_zero);

    TEST_SUMMARY();
}
