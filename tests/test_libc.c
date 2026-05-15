/*
 * Quilon OS -- User libc Unit Tests (section 7.1)
 *
 * Compiled with the native host gcc -- no cross-compiler or QEMU needed.
 *
 * What is tested (pure C, host build):
 *   String functions  -- strlen, strcmp, strncmp, memcpy, memset, memcmp,
 *                        memmove, strncpy, strchr
 *   stdlib: atoi      -- string-to-integer conversion
 *   stdlib: malloc/free -- first-fit block allocator with mock sbrk()
 *   stdio:  printf    -- format specifiers %d, %u, %x, %s, %c, %%
 *                        via a mock write() that captures stdout output
 *
 * What is NOT tested (requires cross-compiled ring-3 execution):
 *   syscall.S         -- int $0x80 stubs (x86 assembly, not runnable on host)
 *   crt0.S            -- startup trampoline
 *   exit()            -- SYS_EXIT syscall
 *
 * Build & run:  cd tests && make
 *
 * Mock strategy
 * -------------
 * framework.h declares:  extern long write(int, const void *, unsigned long)
 * Our mock below matches that signature.  It captures writes to fd=1
 * (stdout) in out_buf so printf tests can inspect the output.  Writes to
 * fd=2 (stderr) go into the same buffer too -- we always reset out_pos
 * before each printf call and NUL-terminate immediately after, so
 * subsequent ASSERT output doesn't corrupt the check.
 *
 * stdlib.c calls sbrk(int increment) -- we provide a mock backed by a
 * 64-KiB static array.
 *
 * NOTE: do NOT include <unistd.h> from user libc here.  That header
 * declares `int write(int, const void *, int)` which conflicts with
 * framework.h's `extern long write(int, const void *, unsigned long)`.
 * We only need <string.h>, <stdio.h>, <stdlib.h> for the tests.
 */

#include "framework.h"

/* User libc headers -- resolved to user/libc/include/ via Makefile -I flag.
 * Intentionally NOT including <unistd.h>; see note above.               */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* -- Mocks -------------------------------------------------------��----------
 *
 * write() -- matches framework.h's extern declaration.
 *   All writes (stdout from printf AND stderr from ASSERT) go into out_buf.
 *   Reset out_pos = 0 before a printf call and NUL-terminate after to
 *   isolate the output under test.
 *
 * sbrk() -- backed by a static 64-KiB heap array.
 * ------------------------------------------------------------------------ */

static char   out_buf[4096];
static int    out_pos = 0;

/*
 * write() mock -- matches framework.h: extern long write(int, const void *, unsigned long)
 *
 * fd=1 (stdout): captured in out_buf for printf assertions.
 * fd=2 (stderr): passed through to the real kernel write syscall so
 *   that framework PASS/FAIL messages appear in the terminal.
 *   We call the syscall directly via inline asm to avoid re-entering
 *   this function (the kernel's write(2) ABI on x86-64 Linux: rax=1,
 *   rdi=fd, rsi=buf, rdx=count, syscall instruction).
 */
long write(int fd, const void *buf, unsigned long len)
{
    if (fd == 1) {
        const char *s = (const char *)buf;
        unsigned long i;
        for (i = 0; i < len && out_pos < (int)(sizeof(out_buf) - 1); i++)
            out_buf[out_pos++] = s[i];
        return (long)len;
    }
    /* Pass through to the real kernel write (x86-64 Linux syscall 1). */
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(1L), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static char mock_heap[65536];
static int  mock_heap_pos = 0;
static int  mock_sbrk_calls = 0;

/* Matches user libc's unistd.h: void *sbrk(int increment) */
void *sbrk(int increment)
{
    mock_sbrk_calls++;
    if (mock_heap_pos + increment < 0 ||
        mock_heap_pos + increment > (int)sizeof(mock_heap))
        return (void *)-1;   /* simulate OOM */
    void *old = &mock_heap[mock_heap_pos];
    mock_heap_pos += increment;
    return old;
}

/* ==============���==============================���=============================
 * 1. String functions
 * =========================================================================== */

static void test_strlen(void)
{
    ASSERT_EQ((int)strlen(""),       0, "strlen empty");
    ASSERT_EQ((int)strlen("a"),      1, "strlen single char");
    ASSERT_EQ((int)strlen("hello"),  5, "strlen hello");
    ASSERT_EQ((int)strlen("abc"),    3, "strlen three chars");
}

static void test_strcmp(void)
{
    ASSERT_EQ(strcmp("",    ""),     0, "strcmp both empty");
    ASSERT_EQ(strcmp("abc", "abc"),  0, "strcmp equal strings");
    ASSERT(   strcmp("abc", "abd") < 0, "strcmp abc < abd");
    ASSERT(   strcmp("abd", "abc") > 0, "strcmp abd > abc");
    ASSERT(   strcmp("ab",  "abc") < 0, "strcmp prefix < full");
    ASSERT(   strcmp("abc", "ab")  > 0, "strcmp full > prefix");
}

static void test_strncmp(void)
{
    ASSERT_EQ(strncmp("abc", "abd", 2), 0,  "strncmp first 2 equal");
    ASSERT(   strncmp("abc", "abd", 3) < 0, "strncmp 3 chars abc<abd");
    ASSERT_EQ(strncmp("xyz", "abc", 0), 0,  "strncmp n=0 always 0");
}

static void test_memcpy(void)
{
    char dst[8] = {0};
    const char src[] = "hello";
    memcpy(dst, src, 6);
    ASSERT_EQ(dst[0], 'h',  "memcpy byte 0");
    ASSERT_EQ(dst[4], 'o',  "memcpy byte 4");
    ASSERT_EQ(dst[5], '\0', "memcpy NUL byte");
}

static void test_memset(void)
{
    char buf[8];
    memset(buf, 0xAB, 8);
    ASSERT_EQ((unsigned char)buf[0], 0xAB, "memset fills byte 0");
    ASSERT_EQ((unsigned char)buf[7], 0xAB, "memset fills byte 7");
    memset(buf, 0, 8);
    ASSERT_EQ((unsigned char)buf[0], 0x00, "memset zero");
}

static void test_memcmp(void)
{
    ASSERT_EQ(memcmp("abc", "abc", 3), 0,  "memcmp equal");
    ASSERT(   memcmp("abc", "abd", 3) < 0, "memcmp first < second");
    ASSERT(   memcmp("abd", "abc", 3) > 0, "memcmp first > second");
    ASSERT_EQ(memcmp("xyz", "abc", 0), 0,  "memcmp n=0 is 0");
}

static void test_memmove(void)
{
    char buf[] = "abcdef";
    memmove(buf + 2, buf + 1, 4);   /* overlapping: shift right */
    ASSERT_EQ(buf[2], 'b', "memmove overlap dst[0]");
    ASSERT_EQ(buf[5], 'e', "memmove overlap dst[3]");

    char a[4] = {1, 2, 3, 4};
    char b[4] = {0};
    memmove(b, a, 4);
    ASSERT_EQ((int)b[0], 1, "memmove non-overlap [0]");
    ASSERT_EQ((int)b[3], 4, "memmove non-overlap [3]");
}

static void test_strncpy(void)
{
    char dst[8];
    memset(dst, 0xFF, 8);
    strncpy(dst, "hi", 8);
    ASSERT_EQ(dst[0], 'h',  "strncpy char 0");
    ASSERT_EQ(dst[1], 'i',  "strncpy char 1");
    ASSERT_EQ(dst[2], '\0', "strncpy NUL pad 2");
    ASSERT_EQ(dst[7], '\0', "strncpy NUL pad 7");
}

static void test_strchr(void)
{
    const char *s = "hello";
    ASSERT(strchr(s, 'e')  == s + 1,       "strchr found 'e'");
    ASSERT(strchr(s, 'z')  == (char *)0,   "strchr not found 'z'");
    ASSERT(strchr(s, '\0') == s + 5,       "strchr NUL terminator");
}

/* =================��=========================================================
 * 2. stdlib: atoi
 * =============================��====================================��======== */

static void test_atoi(void)
{
    ASSERT_EQ(atoi("0"),     0,   "atoi zero");
    ASSERT_EQ(atoi("42"),    42,  "atoi positive");
    ASSERT_EQ(atoi("-17"),  -17,  "atoi negative");
    ASSERT_EQ(atoi("+5"),    5,   "atoi explicit plus");
    ASSERT_EQ(atoi("  10"), 10,   "atoi leading spaces");
    ASSERT_EQ(atoi("99abc"), 99,  "atoi stops at non-digit");
    ASSERT_EQ(atoi(""),      0,   "atoi empty string");
}

/* ===========================================================================
 * 3. stdlib: malloc / free
 *
 * All malloc tests run sequentially on a single heap (heap_head is a
 * static in stdlib.c and persists across test functions).  Tests are
 * designed so each one leaves the heap in a sensible state for the next.
 * ==================================================��======================== */

static void test_malloc_basic(void)
{
    void *p = malloc(16);
    ASSERT(p != (void *)0, "malloc non-NULL");

    void *q = malloc(16);
    ASSERT(q != (void *)0, "second malloc non-NULL");
    ASSERT(p != q,          "two allocations differ");

    memset(p, 0xAA, 16);
    memset(q, 0xBB, 16);
    ASSERT_EQ(((unsigned char *)p)[0], 0xAA, "p data intact after q write");
    ASSERT_EQ(((unsigned char *)q)[0], 0xBB, "q data correct");

    free(p);
    free(q);
}

static void test_malloc_free_reuse(void)
{
    /* After free, first-fit should reuse the freed block. */
    void *p = malloc(32);
    ASSERT(p != (void *)0, "alloc 32 bytes");
    free(p);

    int calls_before = mock_sbrk_calls;
    void *q = malloc(32);
    ASSERT(q != (void *)0, "realloc 32 bytes");
    ASSERT(q == p,          "freed block is reused (first-fit)");
    ASSERT_EQ(mock_sbrk_calls, calls_before, "no extra sbrk on reuse");
    free(q);
}

static void test_malloc_zero(void)
{
    void *p = malloc(0);
    ASSERT(p == (void *)0, "malloc(0) returns NULL");
}

static void test_malloc_growing(void)
{
    void *ptrs[4];
    int   i;
    for (i = 0; i < 4; i++) {
        ptrs[i] = malloc((size_t)(16 * (i + 1)));
        ASSERT(ptrs[i] != (void *)0, "growing alloc non-NULL");
        memset(ptrs[i], (int)(0x10 * (i + 1)), 16 * (i + 1));
    }
    for (i = 0; i < 4; i++) {
        ASSERT_EQ(((unsigned char *)ptrs[i])[0],
                  (int)(unsigned char)(0x10 * (i + 1)),
                  "sentinel byte intact");
        free(ptrs[i]);
    }
}

/* ===========================================================================
 * 4. stdio: printf format specifiers
 *
 * We reset out_pos = 0 before each printf call and NUL-terminate
 * out_buf[out_pos] immediately after.  The ASSERT macros that follow may
 * append to out_buf via fw_write -> write(), but they do so past the NUL
 * terminator and do not affect the strcmp check below.
 * ==============================���=====================================��====== */

static void test_printf_decimal(void)
{
    out_pos = 0; printf("%d", 42);    out_buf[out_pos] = '\0';
    ASSERT_STR_EQ(out_buf, "42",  "printf %d positive");

    out_pos = 0; printf("%d", -7);    out_buf[out_pos] = '\0';
    ASSERT_STR_EQ(out_buf, "-7",  "printf %d negative");

    out_pos = 0; printf("%d", 0);     out_buf[out_pos] = '\0';
    ASSERT_STR_EQ(out_buf, "0",   "printf %d zero");
}

static void test_printf_unsigned(void)
{
    out_pos = 0; printf("%u", 255u);  out_buf[out_pos] = '\0';
    ASSERT_STR_EQ(out_buf, "255", "printf %u 255");

    out_pos = 0; printf("%u", 0u);    out_buf[out_pos] = '\0';
    ASSERT_STR_EQ(out_buf, "0",   "printf %u zero");
}

static void test_printf_hex(void)
{
    out_pos = 0; printf("%x", 0xdead); out_buf[out_pos] = '\0';
    ASSERT_STR_EQ(out_buf, "dead", "printf %x 0xdead");

    out_pos = 0; printf("%x", 0u);    out_buf[out_pos] = '\0';
    ASSERT_STR_EQ(out_buf, "0",   "printf %x zero");
}

static void test_printf_string(void)
{
    out_pos = 0; printf("%s", "hello"); out_buf[out_pos] = '\0';
    ASSERT_STR_EQ(out_buf, "hello",  "printf %s basic");

    out_pos = 0; printf("%s", (char *)0); out_buf[out_pos] = '\0';
    ASSERT_STR_EQ(out_buf, "(null)", "printf %s NULL -> (null)");
}

static void test_printf_char(void)
{
    out_pos = 0; printf("%c", 'Q'); out_buf[out_pos] = '\0';
    ASSERT_EQ(out_buf[0], 'Q', "printf %c");
}

static void test_printf_percent(void)
{
    out_pos = 0; printf("100%%"); out_buf[out_pos] = '\0';
    ASSERT_STR_EQ(out_buf, "100%", "printf %%");
}

static void test_printf_mixed(void)
{
    out_pos = 0;
    printf("pid=%d hex=0x%x str=%s", 3, 0xffu, "ok");
    out_buf[out_pos] = '\0';
    ASSERT_STR_EQ(out_buf, "pid=3 hex=0xff str=ok", "printf mixed format");
}

static void test_printf_width(void)
{
    /* Left-aligned string with field width -- the bug that broke ls */
    out_pos = 0; printf("%-14s|", "hi"); out_buf[out_pos] = '\0';
    ASSERT_STR_EQ(out_buf, "hi            |", "printf %-14s left-align");

    /* Right-aligned string */
    out_pos = 0; printf("%6s|", "hi"); out_buf[out_pos] = '\0';
    ASSERT_STR_EQ(out_buf, "    hi|", "printf %6s right-align");

    /* Right-aligned unsigned with zero-pad */
    out_pos = 0; printf("%05u", 42u); out_buf[out_pos] = '\0';
    ASSERT_STR_EQ(out_buf, "00042", "printf %05u zero-pad");

    /* Width + multiple args -- subsequent args must not be skewed */
    out_pos = 0; printf("%-8s%u", "file.txt", 1234u); out_buf[out_pos] = '\0';
    ASSERT_STR_EQ(out_buf, "file.txt1234", "printf %-8s%u no arg skew");
}

static void test_puts(void)
{
    out_pos = 0;
    puts("world");
    out_buf[out_pos] = '\0';
    ASSERT_EQ(out_buf[0], 'w',  "puts first char");
    ASSERT_EQ(out_buf[5], '\n', "puts appends newline");
}

/* ===========================================================================
 * Runner
 * ========================================��================================��= */

int main(void)
{
    /* String */
    RUN_SUITE(test_strlen);
    RUN_SUITE(test_strcmp);
    RUN_SUITE(test_strncmp);
    RUN_SUITE(test_memcpy);
    RUN_SUITE(test_memset);
    RUN_SUITE(test_memcmp);
    RUN_SUITE(test_memmove);
    RUN_SUITE(test_strncpy);
    RUN_SUITE(test_strchr);

    /* stdlib */
    RUN_SUITE(test_atoi);
    RUN_SUITE(test_malloc_basic);
    RUN_SUITE(test_malloc_free_reuse);
    RUN_SUITE(test_malloc_zero);
    RUN_SUITE(test_malloc_growing);

    /* stdio */
    RUN_SUITE(test_printf_decimal);
    RUN_SUITE(test_printf_unsigned);
    RUN_SUITE(test_printf_hex);
    RUN_SUITE(test_printf_string);
    RUN_SUITE(test_printf_char);
    RUN_SUITE(test_printf_percent);
    RUN_SUITE(test_printf_mixed);
    RUN_SUITE(test_printf_width);
    RUN_SUITE(test_puts);

    TEST_SUMMARY();
}
