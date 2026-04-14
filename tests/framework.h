/*
 * Quilon OS — Test Framework
 *
 * A minimal, zero-dependency test harness for host-side unit tests.
 * Output goes to stderr via write() so it works even when our own
 * printf/puts are the functions being tested.
 *
 * Usage pattern (in each test_*.c file):
 *
 *   static void test_something(void) {
 *       ASSERT_EQ(1 + 1, 2, "basic addition");
 *       ASSERT_STR_EQ(my_func(), "expected", "my_func returns expected");
 *   }
 *
 *   int main(void) {
 *       RUN_SUITE(test_something);
 *       TEST_SUMMARY();   // prints results and returns exit code
 *   }
 */

#ifndef QUILON_TEST_FRAMEWORK_H
#define QUILON_TEST_FRAMEWORK_H

/*
 * Why not #include <unistd.h>?
 *
 * This file is compiled with -I../libc/include, which makes our minimal
 * libc/include/sys/cdefs.h shadow the system one.  The system unistd.h
 * depends on the system cdefs.h for macros like __BEGIN_DECLS and __THROW,
 * so including it here breaks the build.
 *
 * Solution: declare the one function we need (write) by hand.
 * On Linux x86-64, ssize_t == long and size_t == unsigned long, so
 * this declaration is ABI-compatible with the real prototype.
 */
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif
extern long write(int, const void *, unsigned long);

/* ── Pass/fail counters ────────────────────────────────────────────────────
 * These are static so each test binary has its own counters.
 * All suites in one binary share the same counters, giving a
 * combined total in TEST_SUMMARY.
 * ───────────────────────────────────────────────────────────────────────── */
static int fw_passed = 0;
static int fw_failed = 0;

/* ── Raw output helpers ────────────────────────────────────────────────────
 * We cannot use printf here — this file is compiled alongside the OS
 * source files, which define their own printf. Using printf in the
 * framework would call our OS version, which in turn calls our mock
 * putchar, corrupting test output.
 * ───────────────────────────────────────────────────────────────────────── */
static void fw_write(const char *s) {
    const char *p = s;
    while (*p) p++;
    write(STDERR_FILENO, s, (unsigned long)(p - s));
}

static void fw_write_uint(unsigned int n) {
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    if (n == 0) { write(STDERR_FILENO, "0", 1); return; }
    while (n > 0) { buf[--i] = (char)('0' + (n % 10)); n /= 10; }
    fw_write(buf + i);
}

/* ── ANSI colour codes ─────────────────────────────────────────────────── */
#define FW_RESET  "\033[0m"
#define FW_GREEN  "\033[32m"
#define FW_RED    "\033[31m"
#define FW_CYAN   "\033[36m"
#define FW_BOLD   "\033[1m"

/* ── Stringify helpers for __LINE__ in ASSERT output ──────────────────── */
#define FW_STR_(x) #x
#define FW_STR(x)  FW_STR_(x)

/* ── Core assertion ────────────────────────────────────────────────────────
 *
 *   ASSERT(condition, "human readable label")
 *
 * On failure the output includes the file and line number so you can
 * jump straight to the broken assertion.
 * ───────────────────────────────────────────────────────────────────────── */
#define ASSERT(cond, msg)                                              \
    do {                                                               \
        if (cond) {                                                    \
            fw_passed++;                                               \
            fw_write(FW_GREEN "  PASS" FW_RESET ": " msg "\n");       \
        } else {                                                       \
            fw_failed++;                                               \
            fw_write(FW_RED   "  FAIL" FW_RESET ": " msg             \
                     "  (" __FILE__ ":" FW_STR(__LINE__) ")\n");      \
        }                                                              \
    } while (0)

/* ── Typed assertion helpers ───────────────────────────────────────────── */

/* Integer equality / inequality */
#define ASSERT_EQ(a, b, msg)     ASSERT((a) == (b), msg)
#define ASSERT_NE(a, b, msg)     ASSERT((a) != (b), msg)

/* Pointer checks */
#define ASSERT_NULL(p, msg)      ASSERT((void*)(p) == (void*)0, msg)
#define ASSERT_NOTNULL(p, msg)   ASSERT((void*)(p) != (void*)0, msg)

/* Memory block equality — uses memcmp, which must be compiled alongside */
#define ASSERT_MEM_EQ(a, b, n, msg) \
    ASSERT(memcmp((const void*)(a), (const void*)(b), (n)) == 0, msg)

/* Null-terminated string equality — uses fw_streq (no external deps) */
static int fw_streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}
#define ASSERT_STR_EQ(a, b, msg) \
    ASSERT(fw_streq((const char*)(a), (const char*)(b)), msg)

/* ── Suite runner ──────────────────────────────────────────────────────────
 *
 *   RUN_SUITE(my_test_function);
 *
 * Prints a header with the function name then calls the function.
 * ───────────────────────────────────────────────────────────────────────── */
#define RUN_SUITE(fn)                                                  \
    do {                                                               \
        fw_write(FW_BOLD FW_CYAN "\n[" #fn "]\n" FW_RESET);           \
        fn();                                                          \
    } while (0)

/* ── Final summary ─────────────────────────────────────────────────────────
 *
 *   TEST_SUMMARY();
 *
 * Must be the last statement in main(). Prints pass/fail counts,
 * then returns 0 (all pass) or 1 (any failure) from main().
 * ───────────────────────────────────────────────────────────────────────── */
#define TEST_SUMMARY()                                                 \
    do {                                                               \
        fw_write(FW_BOLD "\n--- Results ---\n" FW_RESET);              \
        fw_write("  Passed: "); fw_write_uint((unsigned)fw_passed);    \
        fw_write("\n");                                                \
        fw_write("  Failed: "); fw_write_uint((unsigned)fw_failed);    \
        fw_write("\n");                                                \
        if (fw_failed == 0)                                            \
            fw_write(FW_GREEN FW_BOLD "All tests passed.\n" FW_RESET); \
        else                                                           \
            fw_write(FW_RED FW_BOLD "Some tests FAILED.\n" FW_RESET); \
        return fw_failed > 0 ? 1 : 0;                                 \
    } while (0)

#endif /* QUILON_TEST_FRAMEWORK_H */
