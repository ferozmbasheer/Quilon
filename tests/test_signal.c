/*
 * Quilon OS — Signal Infrastructure Unit Tests (section 6.4)
 *
 * Compiled with the native host gcc — no cross-compiler or QEMU needed.
 *
 * What is tested here (pure C, host build):
 *   • Signal number constants — SIGKILL, SIGSEGV, SIGCHLD values and uniqueness.
 *   • NSIG — covers all defined signals; bitmask fits in uint32_t.
 *   • SIG_DFL / SIG_IGN — sentinel values are distinct and non-NULL/NULL.
 *   • process_t.pending_signals — starts at 0 after process_create().
 *   • process_t.signal_handlers[] — all initialised to SIG_DFL by process_create().
 *   • signal_send() — sets the correct bit in pending_signals.
 *   • signal_send() — multiple sends OR the bits (no clear on re-send).
 *   • signal_send() — out-of-range signum is silently ignored.
 *   • signal_send() — NULL proc is silently ignored.
 *
 * What is NOT tested (requires kernel/hardware):
 *   • signal_dispatch() SIG_DFL path — calls scheduler_yield() → context_switch.
 *   • signal_dispatch() SIG_IGN path — involves current_process global.
 *   • SIGSEGV delivery from exception_handler — requires ring-3 execution.
 *   • User handler stack frame — not yet implemented.
 *
 * Build & run:  cd tests && make
 */

#include "framework.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/signal.h>
#include <kernel/process.h>

/* ── Mocks ──────────────────────────────────────────────────────────────────
 * signal_dispatch() calls scheduler_yield() and printf() when run on-kernel.
 * In the host build, signal.c's dispatch body is guarded by #ifdef __is_kernel,
 * so no mock is needed.
 *
 * process.c calls process_first_run (trampoline) only under __is_kernel;
 * the host build substitutes 0.  No other mocks required.
 * ──────────────────────────────────────────────────────────────────────── */

/* ═══════════════════════════════════════════════════════════════════════════
 * 1. Signal number constants
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_signal_constants(void)
{
    /* Canonical POSIX-compatible values. */
    ASSERT_EQ(SIGKILL,  9,  "SIGKILL == 9");
    ASSERT_EQ(SIGSEGV, 11,  "SIGSEGV == 11");
    ASSERT_EQ(SIGCHLD, 17,  "SIGCHLD == 17");

    /* All must be distinct. */
    ASSERT(SIGKILL != SIGSEGV, "SIGKILL != SIGSEGV");
    ASSERT(SIGKILL != SIGCHLD, "SIGKILL != SIGCHLD");
    ASSERT(SIGSEGV != SIGCHLD, "SIGSEGV != SIGCHLD");

    /* All must fit inside a uint32_t bitmask. */
    ASSERT(SIGKILL  >= 0 && SIGKILL  < NSIG, "SIGKILL  in [0, NSIG)");
    ASSERT(SIGSEGV  >= 0 && SIGSEGV  < NSIG, "SIGSEGV  in [0, NSIG)");
    ASSERT(SIGCHLD  >= 0 && SIGCHLD  < NSIG, "SIGCHLD  in [0, NSIG)");
}

/* ── NSIG ─────────────────────────────────────────────────────────────────── */

static void test_nsig(void)
{
    /* NSIG must fit in 32 bits for the uint32_t bitmask. */
    ASSERT_EQ(NSIG, 32, "NSIG == 32");

    /* All valid signal numbers are in [0, NSIG). */
    ASSERT(SIGKILL  < NSIG, "SIGKILL  < NSIG");
    ASSERT(SIGSEGV  < NSIG, "SIGSEGV  < NSIG");
    ASSERT(SIGCHLD  < NSIG, "SIGCHLD  < NSIG");
}

/* ── SIG_DFL / SIG_IGN ───────────────────────────────────────────────────── */

static void test_sig_dfl_ign(void)
{
    /* SIG_DFL is the null function pointer (0). */
    ASSERT(SIG_DFL == (void (*)(int))0,  "SIG_DFL == NULL fn ptr");

    /* SIG_IGN is the sentinel 1 (non-NULL, non-DFL). */
    ASSERT(SIG_IGN != SIG_DFL,           "SIG_IGN != SIG_DFL");
    ASSERT(SIG_IGN == (void (*)(int))1,  "SIG_IGN == (void(*)(int))1");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 2. process_t signal fields after process_init / process_create
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_process_signal_init(void)
{
    process_init();

    process_t *p = process_create("sig-init-test", 0, 0);
    ASSERT(p != NULL, "process_create: non-NULL");

    ASSERT_EQ(p->pending_signals, 0u,
              "pending_signals initialised to 0");

    for (int s = 0; s < NSIG; s++) {
        ASSERT(p->signal_handlers[s] == SIG_DFL,
               "signal_handlers[s] == SIG_DFL after create");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 3. signal_send — bitmask behaviour
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_signal_send_sets_bit(void)
{
    process_init();
    process_t *p = process_create("send-test", 0, 0);
    ASSERT(p != NULL, "process_create: non-NULL");

    signal_send(p, SIGKILL);

    ASSERT(p->pending_signals & (1u << SIGKILL),
           "signal_send(SIGKILL) sets bit SIGKILL");
    ASSERT(!(p->pending_signals & (1u << SIGSEGV)),
           "signal_send(SIGKILL) does NOT set bit SIGSEGV");
}

static void test_signal_send_multiple(void)
{
    process_init();
    process_t *p = process_create("multi-send", 0, 0);
    ASSERT(p != NULL, "create: non-NULL");

    signal_send(p, SIGKILL);
    signal_send(p, SIGSEGV);

    ASSERT(p->pending_signals & (1u << SIGKILL),
           "after two sends: SIGKILL bit set");
    ASSERT(p->pending_signals & (1u << SIGSEGV),
           "after two sends: SIGSEGV bit set");
    ASSERT_EQ(p->pending_signals,
              (1u << SIGKILL) | (1u << SIGSEGV),
              "exactly two bits set after two sends");
}

static void test_signal_send_idempotent(void)
{
    process_init();
    process_t *p = process_create("idem-test", 0, 0);
    ASSERT(p != NULL, "create: non-NULL");

    signal_send(p, SIGCHLD);
    uint32_t after_first = p->pending_signals;
    signal_send(p, SIGCHLD);

    ASSERT_EQ(p->pending_signals, after_first,
              "sending the same signal twice leaves bitmask unchanged");
}

static void test_signal_send_all_signals(void)
{
    process_init();
    process_t *p = process_create("all-sigs", 0, 0);
    ASSERT(p != NULL, "create: non-NULL");

    uint32_t expected = 0;
    for (int s = 0; s < NSIG; s++) {
        signal_send(p, s);
        expected |= (1u << (unsigned)s);
    }

    ASSERT_EQ(p->pending_signals, expected,
              "all NSIG bits set after sending every signal");
}

/* ── signal_send with invalid arguments ─────────────────────────────────── */

static void test_signal_send_null_proc(void)
{
    /* Sending to NULL must not crash. */
    signal_send(NULL, SIGKILL);   /* must be a no-op */
    ASSERT(1, "signal_send(NULL, SIGKILL) does not crash");
}

static void test_signal_send_out_of_range(void)
{
    process_init();
    process_t *p = process_create("range-test", 0, 0);
    ASSERT(p != NULL, "create: non-NULL");

    signal_send(p, -1);         /* below range */
    signal_send(p, NSIG);       /* equal to NSIG (one past the end) */
    signal_send(p, NSIG + 100); /* way above range */

    ASSERT_EQ(p->pending_signals, 0u,
              "out-of-range sends leave pending_signals unchanged");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 4. signal_handlers array in process_t
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_signal_handlers_size(void)
{
    process_t dummy;
    ASSERT_EQ(sizeof(dummy.signal_handlers),
              (size_t)(NSIG * sizeof(void (*)(int))),
              "signal_handlers[] is exactly NSIG function pointers");
}

static void test_signal_handler_overwrite(void)
{
    process_init();
    process_t *p = process_create("handler-test", 0, 0);
    ASSERT(p != NULL, "create: non-NULL");

    /* Initially SIG_DFL. */
    ASSERT(p->signal_handlers[SIGCHLD] == SIG_DFL,
           "SIGCHLD handler starts SIG_DFL");

    /* Set to SIG_IGN. */
    p->signal_handlers[SIGCHLD] = SIG_IGN;
    ASSERT(p->signal_handlers[SIGCHLD] == SIG_IGN,
           "SIGCHLD handler becomes SIG_IGN after assignment");

    /* Reset to SIG_DFL. */
    p->signal_handlers[SIGCHLD] = SIG_DFL;
    ASSERT(p->signal_handlers[SIGCHLD] == SIG_DFL,
           "SIGCHLD handler reset to SIG_DFL");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 5. Bitmask semantics (signal-independent)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_bitmask_coverage(void)
{
    /* Every signal number in [0, NSIG) must produce a unique non-zero bit. */
    uint32_t seen = 0;
    for (int s = 0; s < NSIG; s++) {
        uint32_t bit = 1u << (unsigned)s;
        ASSERT(bit != 0u,            "bit for signal s is non-zero");
        ASSERT(!(seen & bit),        "bit for signal s is unique");
        seen |= bit;
    }
    /* After all signals, seen should be all-ones (0xFFFFFFFF). */
    ASSERT_EQ(seen, (uint32_t)0xFFFFFFFFu,
              "all 32 signal bits together fill a uint32_t");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    /* Signal constants */
    RUN_SUITE(test_signal_constants);
    RUN_SUITE(test_nsig);
    RUN_SUITE(test_sig_dfl_ign);

    /* Process initialisation */
    RUN_SUITE(test_process_signal_init);

    /* signal_send */
    RUN_SUITE(test_signal_send_sets_bit);
    RUN_SUITE(test_signal_send_multiple);
    RUN_SUITE(test_signal_send_idempotent);
    RUN_SUITE(test_signal_send_all_signals);
    RUN_SUITE(test_signal_send_null_proc);
    RUN_SUITE(test_signal_send_out_of_range);

    /* signal_handlers[] */
    RUN_SUITE(test_signal_handlers_size);
    RUN_SUITE(test_signal_handler_overwrite);

    /* Bitmask */
    RUN_SUITE(test_bitmask_coverage);

    TEST_SUMMARY();
}
