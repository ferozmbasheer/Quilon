/*
 * Quilon OS — String Library Tests
 *
 * Tests every function in libc/string/.
 * Compiled with the host gcc — no cross-compiler or QEMU needed.
 *
 * Build & run:  make (from tests/)
 */

#include "framework.h"
#include <string.h>   /* our libc/include/string.h */

/* ═══════════════════════════════════════════════════════════════
 * memcpy
 * ═══════════════════════════════════════════════════════════════ */
static void test_memcpy(void)
{
    /* Basic copy including null terminator */
    char src[] = "hello";
    char dst[6] = {0};
    memcpy(dst, src, 6);
    ASSERT_STR_EQ(dst, "hello", "copies a normal string");

    /* Destination is returned */
    char x[4];
    ASSERT_EQ(memcpy(x, "abc", 4), (void *)x, "returns dst pointer");

    /* Zero-length copy must not touch dst */
    char buf[4] = {1, 2, 3, 4};
    memcpy(buf, "\xFF\xFF\xFF\xFF", 0);
    ASSERT_EQ(buf[0], 1, "zero-length: dst[0] unchanged");
    ASSERT_EQ(buf[3], 4, "zero-length: dst[3] unchanged");

    /* Single byte */
    char a = 0;
    char b = 42;
    memcpy(&a, &b, 1);
    ASSERT_EQ(a, 42, "single-byte copy");

    /* Copies binary data (not just printable text) */
    unsigned char binary_src[4] = {0x00, 0xFF, 0x7F, 0x80};
    unsigned char binary_dst[4] = {0};
    memcpy(binary_dst, binary_src, 4);
    ASSERT_MEM_EQ(binary_dst, binary_src, 4, "copies binary data correctly");
}

/* ═══════════════════════════════════════════════════════════════
 * memmove
 * ═══════════════════════════════════════════════════════════════ */
static void test_memmove(void)
{
    /* No overlap — behaves like memcpy */
    char s[12] = "hello";
    memmove(s + 6, s, 6);
    ASSERT_MEM_EQ(s + 6, "hello", 6, "no-overlap copy");

    /* Forward overlap: dst > src (copy right — must go back-to-front) */
    char fwd[8] = "abcde";
    memmove(fwd + 2, fwd, 4);   /* "abcd" → fwd+2, giving "ababcd" */
    ASSERT_MEM_EQ(fwd, "ababcd", 6, "forward overlap preserved");

    /* Backward overlap: dst < src (copy left — must go front-to-back) */
    char bwd[8] = "abcde";
    memmove(bwd, bwd + 1, 4);   /* shift left by 1: "bcde" */
    ASSERT_MEM_EQ(bwd, "bcde", 4, "backward overlap preserved");

    /* Exact same pointer — no-op, returns dst */
    char same[4] = {9, 8, 7, 6};
    ASSERT_EQ(memmove(same, same, 4), (void *)same, "same src==dst returns dst");
    ASSERT_EQ(same[0], 9, "same src==dst: data unchanged");

    /* Zero length — must not modify dst */
    char z[4] = {0, 1, 2, 3};
    memmove(z + 1, z, 0);
    ASSERT_EQ(z[1], 1, "zero-length: dst unchanged");
}

/* ═══════════════════════════════════════════════════════════════
 * memset
 * ═══════════════════════════════════════════════════════════════ */
static void test_memset(void)
{
    char buf[8];

    /* Fill with a non-zero value */
    memset(buf, 'A', 8);
    ASSERT_EQ(buf[0], 'A', "fills first byte");
    ASSERT_EQ(buf[7], 'A', "fills last byte");

    /* Fill with zero */
    memset(buf, 0, 8);
    ASSERT_EQ(buf[0], 0, "zero-fill first byte");
    ASSERT_EQ(buf[7], 0, "zero-fill last byte");

    /* Only value's low byte is used (per C standard) */
    memset(buf, 0x1FF, 1);   /* 0x1FF truncated to 0xFF */
    ASSERT_EQ((unsigned char)buf[0], 0xFF, "only low byte of value is used");

    /* Zero length — must not touch dst */
    char keep = 99;
    memset(&keep, 0, 0);
    ASSERT_EQ(keep, 99, "zero-length: value unchanged");

    /* Returns dst pointer */
    ASSERT_EQ(memset(buf, 0, 8), (void *)buf, "returns dst pointer");
}

/* ═══════════════════════════════════════════════════════════════
 * memcmp
 * ═══════════════════════════════════════════════════════════════ */
static void test_memcmp(void)
{
    /* Equal regions */
    ASSERT_EQ(memcmp("abc", "abc", 3), 0, "equal: returns 0");

    /* Strictly less */
    ASSERT_EQ(memcmp("abc", "abd", 3), -1, "less-than: returns -1");

    /* Strictly greater */
    ASSERT_EQ(memcmp("abd", "abc", 3), 1, "greater-than: returns 1");

    /* Only compare n bytes — prefix match */
    ASSERT_EQ(memcmp("abc", "abcd", 3), 0, "prefix equal with n=3 returns 0");

    /* Zero length is always equal */
    ASSERT_EQ(memcmp("abc", "xyz", 0), 0, "zero-length: returns 0");

    /* High-byte values compare as unsigned chars */
    unsigned char hi[1] = {0xFF};
    unsigned char lo[1] = {0x00};
    ASSERT_EQ(memcmp(hi, lo, 1), 1, "0xFF > 0x00 (unsigned comparison)");
    ASSERT_EQ(memcmp(lo, hi, 1), -1, "0x00 < 0xFF");

    /* Difference at last byte */
    ASSERT_EQ(memcmp("aac", "aab", 3), 1, "difference at last byte");
}

/* ═══════════════════════════════════════════════════════════════
 * strlen
 * ═══════════════════════════════════════════════════════════════ */
static void test_strlen(void)
{
    ASSERT_EQ(strlen(""),          (size_t)0,  "empty string → 0");
    ASSERT_EQ(strlen("a"),         (size_t)1,  "single char → 1");
    ASSERT_EQ(strlen("hello"),     (size_t)5,  "normal string → 5");
    ASSERT_EQ(strlen("hello\0x"),  (size_t)5,  "stops at first null");
    ASSERT_EQ(strlen("ab\0cd"),    (size_t)2,  "embedded null terminates count");

    /* String with spaces and symbols */
    ASSERT_EQ(strlen("a b c"),     (size_t)5,  "counts spaces");
    ASSERT_EQ(strlen("1234567890"), (size_t)10, "ten-char string");
}

/* ═══════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════ */
int main(void)
{
    RUN_SUITE(test_memcpy);
    RUN_SUITE(test_memmove);
    RUN_SUITE(test_memset);
    RUN_SUITE(test_memcmp);
    RUN_SUITE(test_strlen);
    TEST_SUMMARY();
}
