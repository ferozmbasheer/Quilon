/*
 * Quilon OS — System Call Unit Tests
 *
 * Compiled with the native host gcc — no cross-compiler or QEMU needed.
 *
 * What is tested here:
 *   Constants  — SYS_* numbers and FD_* values are correct and non-overlapping.
 *   Struct layout — syscall_regs_t fields sit at the expected offsets (must
 *                   match the stack frame int80_stub builds in boot.S).
 *   Handler dispatch — syscall_handler correctly routes each syscall number,
 *                      writes the return value into regs->eax, and calls
 *                      terminal_write with the right arguments for SYS_WRITE.
 *
 * What is NOT tested here (requires x86 hardware / QEMU):
 *   syscall_initialize() — calls idt_set_gate(), which writes into the IDT
 *                          array only present on a real/emulated x86 CPU.
 *   int80_stub            — inline assembly (int $0x80, iret) can only run
 *                          on a 32-bit x86 kernel.
 *   SYS_EXIT halt         — the `hlt` instruction is guarded by #ifdef
 *                          __is_kernel; on the host SYS_EXIT returns normally
 *                          so the test can inspect the result.
 *
 * Mocks
 * ─────
 * terminal_write — captured to a fixed buffer; call count and last length
 *                  are exposed via file-scope variables.
 * putchar        — captured to a separate buffer so printf() output from
 *                  inside syscall_handler can be examined or ignored.
 *                  putchar.c is excluded from the link (see Makefile) so
 *                  this definition wins.
 *
 * Build & run:  cd tests && make
 */

#include "framework.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/syscall.h>

/* ── Mocks ──────────────────────────────────────────────────────────────────
 * terminal_write and putchar are called by syscall_handler.
 * We provide lightweight stubs here; tty.c and putchar.c are not linked.
 * ──────────────────────────────────────────────────────────────────────── */

/* terminal_write mock ─── records call count and length only.
 *
 * The `data` pointer stored in regs->ecx is a uint32_t, so on a 64-bit host
 * it may be a truncated pointer (the upper 32 bits of the host stack address
 * are lost).  Dereferencing it would crash.  We therefore do NOT read the
 * buffer contents here — we only verify that terminal_write was called and
 * received the right length.  This is the same pointer-truncation caveat
 * documented in test_pmm.c for pmm_initialize().                            */
static int    tw_calls      = 0;
static size_t tw_last_len   = 0;

void terminal_write(const char *data, size_t size)
{
    (void)data;   /* do not dereference — may be a truncated 32-bit pointer */
    tw_calls++;
    tw_last_len = size;
}

static void tw_reset(void) { tw_calls = 0; tw_last_len = 0; }

/* putchar mock ─── absorbs printf output from syscall_handler */
int putchar(int c)
{
    (void)c;
    return c;
}

/* Helper: build a zeroed syscall_regs_t with a given syscall number */
static syscall_regs_t make_regs(uint32_t syscall_no)
{
    syscall_regs_t r;
    /* zero every field */
    for (size_t i = 0; i < sizeof(r); i++)
        ((unsigned char *)&r)[i] = 0;
    r.eax = syscall_no;
    return r;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 1. Syscall number constants
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_syscall_numbers(void)
{
    /* Values must be stable — user-space ABI depends on them */
    ASSERT_EQ(SYS_WRITE,  1u, "SYS_WRITE  == 1");
    ASSERT_EQ(SYS_GETPID, 2u, "SYS_GETPID == 2");
    ASSERT_EQ(SYS_EXIT,   3u, "SYS_EXIT   == 3");

    /* No two syscall numbers may be equal */
    ASSERT(SYS_WRITE  != SYS_GETPID, "SYS_WRITE  != SYS_GETPID");
    ASSERT(SYS_WRITE  != SYS_EXIT,   "SYS_WRITE  != SYS_EXIT");
    ASSERT(SYS_GETPID != SYS_EXIT,   "SYS_GETPID != SYS_EXIT");

    /* All defined syscall numbers must be positive */
    ASSERT(SYS_WRITE  > 0u, "SYS_WRITE  > 0");
    ASSERT(SYS_GETPID > 0u, "SYS_GETPID > 0");
    ASSERT(SYS_EXIT   > 0u, "SYS_EXIT   > 0");
}

/* ── File descriptor constants ─────────────────────────────────────────── */

static void test_fd_constants(void)
{
    ASSERT_EQ(FD_STDIN,  0u, "FD_STDIN  == 0");
    ASSERT_EQ(FD_STDOUT, 1u, "FD_STDOUT == 1");
    ASSERT_EQ(FD_STDERR, 2u, "FD_STDERR == 2");

    /* Must be distinct */
    ASSERT(FD_STDIN != FD_STDOUT, "FD_STDIN  != FD_STDOUT");
    ASSERT(FD_STDIN != FD_STDERR, "FD_STDIN  != FD_STDERR");
    ASSERT(FD_STDOUT != FD_STDERR, "FD_STDOUT != FD_STDERR");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 2. syscall_regs_t struct layout
 *
 * The offsets here must exactly match the stack layout that int80_stub
 * builds in boot.S.  A mismatch would silently corrupt registers on every
 * system call — this test catches that class of bug before running on QEMU.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_regs_struct_layout(void)
{
    /* 14 fields × 4 bytes each:
     *   ds(1) + pusha save area(8: edi,esi,ebp,esp,ebx,edx,ecx,eax)
     *   + int_no,err_code(2) + eip,cs,eflags(3) = 14 */
    ASSERT_EQ(sizeof(syscall_regs_t), (size_t)(14 * 4),
              "syscall_regs_t is 14 × 4 = 56 bytes");

    /* Offsets: ds, then pusha area (edi..eax = 9 words), then int_no, err_code,
     * then CPU-pushed eip, cs, eflags.                                        */
    ASSERT_EQ(offsetof(syscall_regs_t, ds),       (size_t) 0, "ds       @ offset  0");
    ASSERT_EQ(offsetof(syscall_regs_t, edi),      (size_t) 4, "edi      @ offset  4");
    ASSERT_EQ(offsetof(syscall_regs_t, esi),      (size_t) 8, "esi      @ offset  8");
    ASSERT_EQ(offsetof(syscall_regs_t, ebp),      (size_t)12, "ebp      @ offset 12");
    ASSERT_EQ(offsetof(syscall_regs_t, esp),      (size_t)16, "esp      @ offset 16");
    ASSERT_EQ(offsetof(syscall_regs_t, ebx),      (size_t)20, "ebx      @ offset 20");
    ASSERT_EQ(offsetof(syscall_regs_t, edx),      (size_t)24, "edx      @ offset 24");
    ASSERT_EQ(offsetof(syscall_regs_t, ecx),      (size_t)28, "ecx      @ offset 28");
    ASSERT_EQ(offsetof(syscall_regs_t, eax),      (size_t)32, "eax      @ offset 32");
    ASSERT_EQ(offsetof(syscall_regs_t, int_no),   (size_t)36, "int_no   @ offset 36");
    ASSERT_EQ(offsetof(syscall_regs_t, err_code), (size_t)40, "err_code @ offset 40");
    ASSERT_EQ(offsetof(syscall_regs_t, eip),      (size_t)44, "eip      @ offset 44");
    ASSERT_EQ(offsetof(syscall_regs_t, cs),       (size_t)48, "cs       @ offset 48");
    ASSERT_EQ(offsetof(syscall_regs_t, eflags),   (size_t)52, "eflags   @ offset 52");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 3. SYS_WRITE dispatch
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_sys_write_stdout(void)
{
    tw_reset();

    const char msg[] = "hello kernel";
    syscall_regs_t r = make_regs(SYS_WRITE);
    r.ebx = FD_STDOUT;
    r.ecx = (uint32_t)(uintptr_t)msg;
    r.edx = (uint32_t)(sizeof(msg) - 1);

    syscall_handler(&r);

    ASSERT_EQ(tw_calls, 1,
              "SYS_WRITE to stdout calls terminal_write once");
    ASSERT_EQ(tw_last_len, sizeof(msg) - 1,
              "SYS_WRITE passes the correct length to terminal_write");
    ASSERT_EQ(r.eax, (uint32_t)(sizeof(msg) - 1),
              "SYS_WRITE return value equals bytes written");
    /* Note: buffer contents are not verified here — the buf pointer is stored
     * as uint32_t in regs->ecx and would be a truncated address on 64-bit. */
}

static void test_sys_write_stderr(void)
{
    tw_reset();

    const char msg[] = "error msg";
    syscall_regs_t r = make_regs(SYS_WRITE);
    r.ebx = FD_STDERR;
    r.ecx = (uint32_t)(uintptr_t)msg;
    r.edx = (uint32_t)(sizeof(msg) - 1);

    syscall_handler(&r);

    ASSERT_EQ(tw_calls, 1,
              "SYS_WRITE to stderr calls terminal_write");
    ASSERT_EQ(r.eax, (uint32_t)(sizeof(msg) - 1),
              "SYS_WRITE to stderr returns length");
}

static void test_sys_write_stdin_ignored(void)
{
    tw_reset();

    const char msg[] = "ignored";
    syscall_regs_t r = make_regs(SYS_WRITE);
    r.ebx = FD_STDIN;  /* writing to stdin is not supported */
    r.ecx = (uint32_t)(uintptr_t)msg;
    r.edx = (uint32_t)(sizeof(msg) - 1);

    syscall_handler(&r);

    ASSERT_EQ(tw_calls, 0,
              "SYS_WRITE to stdin does not call terminal_write");
    ASSERT_EQ(r.eax, 0u,
              "SYS_WRITE to stdin returns 0");
}

static void test_sys_write_null_buf(void)
{
    tw_reset();

    syscall_regs_t r = make_regs(SYS_WRITE);
    r.ebx = FD_STDOUT;
    r.ecx = 0;          /* NULL pointer */
    r.edx = 10;

    syscall_handler(&r);

    ASSERT_EQ(tw_calls, 0,
              "SYS_WRITE with NULL buffer does not call terminal_write");
    ASSERT_EQ(r.eax, 0u,
              "SYS_WRITE with NULL buffer returns 0");
}

static void test_sys_write_zero_len(void)
{
    tw_reset();

    const char msg[] = "zero";
    syscall_regs_t r = make_regs(SYS_WRITE);
    r.ebx = FD_STDOUT;
    r.ecx = (uint32_t)(uintptr_t)msg;
    r.edx = 0;           /* zero bytes requested */

    syscall_handler(&r);

    ASSERT_EQ(tw_calls, 1,
              "SYS_WRITE with len=0 still calls terminal_write");
    ASSERT_EQ(r.eax, 0u,
              "SYS_WRITE with len=0 returns 0");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 4. SYS_GETPID dispatch
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_sys_getpid(void)
{
    syscall_regs_t r = make_regs(SYS_GETPID);

    syscall_handler(&r);

    ASSERT_EQ(r.eax, 0u,
              "SYS_GETPID returns 0 (single-task kernel has only one process)");
}

static void test_sys_getpid_does_not_touch_terminal(void)
{
    tw_reset();

    syscall_regs_t r = make_regs(SYS_GETPID);
    syscall_handler(&r);

    ASSERT_EQ(tw_calls, 0,
              "SYS_GETPID does not call terminal_write");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 5. SYS_EXIT dispatch  (host build: hlt is skipped, handler returns)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_sys_exit_returns_zero(void)
{
    syscall_regs_t r = make_regs(SYS_EXIT);
    r.ebx = 0; /* exit code */

    syscall_handler(&r);  /* on host: prints message, sets eax=0, returns */

    ASSERT_EQ(r.eax, 0u,
              "SYS_EXIT sets eax=0 on host build");
}

static void test_sys_exit_nonzero_code(void)
{
    syscall_regs_t r = make_regs(SYS_EXIT);
    r.ebx = 42; /* non-zero exit code — handler should still return on host */

    syscall_handler(&r);

    ASSERT_EQ(r.eax, 0u,
              "SYS_EXIT with non-zero code still returns eax=0 on host");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 6. Unknown syscall
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_unknown_syscall_returns_minus1(void)
{
    syscall_regs_t r = make_regs(0xDEAD);

    syscall_handler(&r);

    ASSERT_EQ(r.eax, (uint32_t)-1,
              "unknown syscall returns (uint32_t)-1 = ENOSYS");
}

static void test_syscall_zero_returns_minus1(void)
{
    syscall_regs_t r = make_regs(0);

    syscall_handler(&r);

    ASSERT_EQ(r.eax, (uint32_t)-1,
              "syscall 0 (undefined) returns ENOSYS");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 7. regs->eax is always written (no stale caller value)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_eax_is_always_overwritten(void)
{
    /* Set eax to a sentinel value before the call.  After the call it must
     * have been replaced by the handler — never read the caller's value.  */
    const uint32_t SENTINEL = 0xCAFEBABEu;

    syscall_regs_t r;
    for (size_t i = 0; i < sizeof(r); i++)
        ((unsigned char *)&r)[i] = 0;
    r.eax = SENTINEL;        /* syscall 0xCAFEBABE — undefined */

    syscall_handler(&r);

    ASSERT(r.eax != SENTINEL,
           "eax is overwritten by handler (sentinel value gone)");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    /* Constants */
    RUN_SUITE(test_syscall_numbers);
    RUN_SUITE(test_fd_constants);

    /* Struct layout */
    RUN_SUITE(test_regs_struct_layout);

    /* SYS_WRITE */
    RUN_SUITE(test_sys_write_stdout);
    RUN_SUITE(test_sys_write_stderr);
    RUN_SUITE(test_sys_write_stdin_ignored);
    RUN_SUITE(test_sys_write_null_buf);
    RUN_SUITE(test_sys_write_zero_len);

    /* SYS_GETPID */
    RUN_SUITE(test_sys_getpid);
    RUN_SUITE(test_sys_getpid_does_not_touch_terminal);

    /* SYS_EXIT */
    RUN_SUITE(test_sys_exit_returns_zero);
    RUN_SUITE(test_sys_exit_nonzero_code);

    /* Unknown syscalls */
    RUN_SUITE(test_unknown_syscall_returns_minus1);
    RUN_SUITE(test_syscall_zero_returns_minus1);

    /* General correctness */
    RUN_SUITE(test_eax_is_always_overwritten);

    TEST_SUMMARY();
}
