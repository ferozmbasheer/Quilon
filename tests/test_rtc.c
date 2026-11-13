/*
 * Quilon OS -- CMOS RTC decoding + calendar tests (ROADMAP3 18.4)
 *
 * Drives the pure half of kernel/kernel/rtc.c: raw CMOS registers in every
 * format the chip can be set to (BCD/binary x 12h/24h) -> calendar time, and
 * calendar time -> Unix seconds.
 */

#include "framework.h"
#include <kernel/rtc.h>

#define RAW(s, m, h, d, mo, y) ((const uint8_t[RTC_NREGS]){ s, m, h, d, mo, y })

static void test_bcd_24h(void)
{
    rtc_time_t t;
    /* QEMU's default: BCD, 24-hour.  2026-11-12 21:47:05. */
    ASSERT_EQ(rtc_decode(RAW(0x05, 0x47, 0x21, 0x12, 0x11, 0x26), RTC_B_24H, &t),
              0, "valid");
    ASSERT_EQ(t.year, 2026, "year");
    ASSERT_EQ(t.mon, 11, "month");
    ASSERT_EQ(t.day, 12, "day");
    ASSERT_EQ(t.hour, 21, "hour");
    ASSERT_EQ(t.min, 47, "min");
    ASSERT_EQ(t.sec, 5, "sec");
}

static void test_binary_24h(void)
{
    rtc_time_t t;
    ASSERT_EQ(rtc_decode(RAW(59, 59, 23, 31, 12, 99),
                         RTC_B_24H | RTC_B_BINARY, &t), 0, "valid");
    ASSERT_EQ(t.year, 2099, "year");
    ASSERT_EQ(t.hour, 23, "hour");
    ASSERT_EQ(t.sec, 59, "sec");
}

static void test_12h(void)
{
    rtc_time_t t;
    /* BCD 12-hour: 12 AM -> 0, 12 PM -> 12, 9 PM -> 21. */
    rtc_decode(RAW(0, 0, 0x12, 1, 1, 0x26), 0, &t);
    ASSERT_EQ(t.hour, 0, "12 AM = 00h");
    rtc_decode(RAW(0, 0, 0x12 | RTC_HOUR_PM, 1, 1, 0x26), 0, &t);
    ASSERT_EQ(t.hour, 12, "12 PM = 12h");
    rtc_decode(RAW(0, 0, 0x09 | RTC_HOUR_PM, 1, 1, 0x26), 0, &t);
    ASSERT_EQ(t.hour, 21, "9 PM = 21h");
    /* Binary 12-hour. */
    rtc_decode(RAW(0, 0, 11 | RTC_HOUR_PM, 1, 1, 26), RTC_B_BINARY, &t);
    ASSERT_EQ(t.hour, 23, "binary 11 PM = 23h");
    ASSERT_EQ(rtc_decode(RAW(0, 0, 0x00, 1, 1, 0x26), 0, &t), -1,
              "12-hour mode has no hour 0");
}

static void test_garbage_rejected(void)
{
    rtc_time_t t;
    ASSERT_EQ(rtc_decode(RAW(0x60, 0, 0, 1, 1, 0x26), RTC_B_24H, &t), -1, "sec 60");
    ASSERT_EQ(rtc_decode(RAW(0, 0, 0x24, 1, 1, 0x26), RTC_B_24H, &t), -1, "hour 24");
    ASSERT_EQ(rtc_decode(RAW(0, 0, 0, 0, 1, 0x26), RTC_B_24H, &t), -1, "day 0");
    ASSERT_EQ(rtc_decode(RAW(0, 0, 0, 1, 0x13, 0x26), RTC_B_24H, &t), -1, "month 13");
    ASSERT_EQ(rtc_decode(RAW(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF), RTC_B_24H, &t),
              -1, "all-ones (no RTC) rejected");
}

static void test_to_unix(void)
{
    rtc_time_t t = { 1970, 1, 1, 0, 0, 0 };
    ASSERT_EQ(rtc_to_unix(&t), 0u, "epoch");
    t = (rtc_time_t){ 2000, 1, 1, 0, 0, 0 };
    ASSERT_EQ(rtc_to_unix(&t), 946684800u, "2000-01-01");
    t = (rtc_time_t){ 2000, 3, 1, 0, 0, 0 };
    ASSERT_EQ(rtc_to_unix(&t), 951868800u, "2000-03-01 (after a leap Feb 29)");
    t = (rtc_time_t){ 2024, 2, 29, 12, 0, 0 };
    ASSERT_EQ(rtc_to_unix(&t), 1709208000u, "2024-02-29 12:00");
    t = (rtc_time_t){ 2026, 11, 12, 21, 47, 5 };
    ASSERT_EQ(rtc_to_unix(&t), 1794520025u, "2026-11-12 21:47:05");
    t = (rtc_time_t){ 2099, 12, 31, 23, 59, 59 };
    ASSERT_EQ(rtc_to_unix(&t), 4102444799u, "2099-12-31 23:59:59");
}

int main(void)
{
    RUN_SUITE(test_bcd_24h);
    RUN_SUITE(test_binary_24h);
    RUN_SUITE(test_12h);
    RUN_SUITE(test_garbage_rejected);
    RUN_SUITE(test_to_unix);
    TEST_SUMMARY();
}
