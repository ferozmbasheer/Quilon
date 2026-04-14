/*
 * Quilon OS — printf Tests
 *
 * How the mock works
 * ──────────────────
 * printf.c calls putchar() for every character it outputs.
 * Normally putchar() calls terminal_write() (VGA hardware).
 * Here we provide our own putchar() that writes into a local
 * buffer instead.  putchar.c is deliberately NOT compiled into
 * this binary — the linker uses our version below.
 *
 * This means every call to printf() during a test is fully
 * captured in out_buf[], and we can ASSERT on the exact string
 * that would have appeared on screen.
 *
 * Known-failing tests
 * ───────────────────
 * Tests marked "(KNOWN BUG)" are expected to fail until the bug
 * described in ROADMAP.md §2 is fixed.  They are included so that
 * fixing the bug immediately turns a red test green — the test
 * suite is your feedback loop.
 *
 * Build & run:  make (from tests/)
 */

#include "framework.h"

/*
 * Include our libc headers AFTER framework.h.
 * -I../libc/include ensures we get our stdio.h / string.h,
 * not the system ones.
 */
#include <stdio.h>    /* declares printf, putchar */
#include <string.h>   /* declares memset — needed for reset_buf() */

/* ── Output capture buffer ─────────────────────────────────────────────── */

static char out_buf[512];
static int  out_pos;

/*
 * Our mock putchar — replaces the VGA one from putchar.c.
 * printf.c calls this for every character it outputs.
 */
int putchar(int ic)
{
    if (out_pos < (int)(sizeof(out_buf) - 1))
        out_buf[out_pos++] = (char)ic;
    return ic;
}

/* Reset the capture buffer between tests */
static void reset_buf(void)
{
    out_pos = 0;
    memset(out_buf, 0, sizeof(out_buf));
}

/* ═══════════════════════════════════════════════════════════════
 * %d — integer formatting
 * ═══════════════════════════════════════════════════════════════ */
static void test_printf_d(void)
{
    reset_buf(); printf("%d", 1);
    ASSERT_STR_EQ(out_buf, "1", "%d: single digit");

    reset_buf(); printf("%d", 42);
    ASSERT_STR_EQ(out_buf, "42", "%d: two digits");

    reset_buf(); printf("%d", 100);
    ASSERT_STR_EQ(out_buf, "100", "%d: three digits");

    reset_buf(); printf("%d", 2147483647);
    ASSERT_STR_EQ(out_buf, "2147483647", "%d: INT_MAX");

    /*
     * KNOWN BUG — see ROADMAP.md §2 Bug 1
     * The atoi() helper in printf.c uses `while(copy)` which never
     * executes when copy == 0, so nothing is printed for zero.
     * Fix it, then this test turns green.
     */
    reset_buf(); printf("%d", 0);
    ASSERT_STR_EQ(out_buf, "0", "%d: zero (KNOWN BUG — fix atoi in printf.c)");

    /*
     * KNOWN BUG — negative numbers also broken (no sign handling in atoi)
     */
    reset_buf(); printf("%d", -1);
    ASSERT_STR_EQ(out_buf, "-1", "%d: negative -1 (KNOWN BUG — fix atoi in printf.c)");

    reset_buf(); printf("%d", -42);
    ASSERT_STR_EQ(out_buf, "-42", "%d: negative -42 (KNOWN BUG — fix atoi in printf.c)");
}

/* ═══════════════════════════════════════════════════════════════
 * %s — string formatting
 * ═══════════════════════════════════════════════════════════════ */
static void test_printf_s(void)
{
    reset_buf(); printf("%s", "hello");
    ASSERT_STR_EQ(out_buf, "hello", "%s: normal string");

    reset_buf(); printf("%s", "");
    ASSERT_STR_EQ(out_buf, "", "%s: empty string");

    reset_buf(); printf("prefix %s suffix", "mid");
    ASSERT_STR_EQ(out_buf, "prefix mid suffix", "%s: surrounded by literal text");

    reset_buf(); printf("%s", "a b c");
    ASSERT_STR_EQ(out_buf, "a b c", "%s: string with spaces");
}

/* ═══════════════════════════════════════════════════════════════
 * %c — character formatting
 * ═══════════════════════════════════════════════════════════════ */
static void test_printf_c(void)
{
    reset_buf(); printf("%c", 'A');
    ASSERT_EQ((unsigned char)out_buf[0], 'A', "%c: uppercase letter");

    reset_buf(); printf("%c", '0');
    ASSERT_EQ((unsigned char)out_buf[0], '0', "%c: digit character");

    reset_buf(); printf("%c", ' ');
    ASSERT_EQ((unsigned char)out_buf[0], ' ', "%c: space character");
}

/* ═══════════════════════════════════════════════════════════════
 * %% — literal percent
 * ═══════════════════════════════════════════════════════════════ */
static void test_printf_percent(void)
{
    reset_buf(); printf("%%");
    ASSERT_STR_EQ(out_buf, "%", "%% alone");

    reset_buf(); printf("100%%");
    ASSERT_STR_EQ(out_buf, "100%", "%% after digits");

    reset_buf(); printf("50%% off");
    ASSERT_STR_EQ(out_buf, "50% off", "%% surrounded by text");
}

/* ═══════════════════════════════════════════════════════════════
 * Mixed arguments
 * ═══════════════════════════════════════════════════════════════ */
static void test_printf_mixed(void)
{
    /* This is the call from kernel.c — verify it produces the right output */
    reset_buf(); printf("%c %s %d", 'c', "this", 42);
    ASSERT_STR_EQ(out_buf, "c this 42", "mixed: char + string + int");

    reset_buf(); printf("%s=%d", "count", 7);
    ASSERT_STR_EQ(out_buf, "count=7", "mixed: string + int");
}

/* ═══════════════════════════════════════════════════════════════
 * Literal text (no format specifiers)
 * ═══════════════════════════════════════════════════════════════ */
static void test_printf_literal(void)
{
    reset_buf(); printf("hello");
    ASSERT_STR_EQ(out_buf, "hello", "literal string");

    reset_buf(); printf("Quilon OS v0.0.1\r\n");
    ASSERT_STR_EQ(out_buf, "Quilon OS v0.0.1\r\n", "literal with CRLF");
}

/* ═══════════════════════════════════════════════════════════════
 * Return value (number of characters written)
 * ═══════════════════════════════════════════════════════════════ */
static void test_printf_return(void)
{
    reset_buf();
    int n = printf("hello");
    ASSERT_EQ(n, 5, "return value equals number of characters written");

    reset_buf();
    n = printf("%s", "abc");
    ASSERT_EQ(n, 3, "return value for %%s");
}

/* ═══════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════ */
int main(void)
{
    RUN_SUITE(test_printf_d);
    RUN_SUITE(test_printf_s);
    RUN_SUITE(test_printf_c);
    RUN_SUITE(test_printf_percent);
    RUN_SUITE(test_printf_mixed);
    RUN_SUITE(test_printf_literal);
    RUN_SUITE(test_printf_return);
    TEST_SUMMARY();
}
