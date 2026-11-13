/*
 * Quilon OS -- CMOS real-time clock (ROADMAP3 18.4: the taskbar tray clock)
 *
 * The PC's battery-backed clock, read through CMOS ports 0x70/0x71.  QEMU
 * keeps it in UTC.  The decoding and calendar maths are pure C and
 * host-tested (tests/test_rtc.c); only rtc_now() touches the hardware.
 */
#ifndef _KERNEL_RTC_H
#define _KERNEL_RTC_H

#include <stdint.h>

typedef struct {
    int year;      /* full year, e.g. 2026 */
    int mon;       /* 1..12 */
    int day;       /* 1..31 */
    int hour;      /* 0..23 */
    int min;       /* 0..59 */
    int sec;       /* 0..59 */
} rtc_time_t;

/* Indices into the raw register array rtc_decode takes. */
enum { RTC_R_SEC, RTC_R_MIN, RTC_R_HOUR, RTC_R_DAY, RTC_R_MON, RTC_R_YEAR,
       RTC_NREGS };

/* CMOS status register B bits. */
#define RTC_B_24H     0x02   /* set: 24-hour mode; clear: 12-hour + PM bit */
#define RTC_B_BINARY  0x04   /* set: binary values; clear: BCD             */
#define RTC_HOUR_PM   0x80   /* 12-hour mode: PM flag in the hour register */

/* Turn raw CMOS registers (as read, BCD or binary, 12- or 24-hour, per
 * status register B) into a calendar time.  The two-digit year is taken as
 * 20yy.  Returns 0, or -1 if any field is out of range (garbage CMOS). */
int rtc_decode(const uint8_t raw[RTC_NREGS], uint8_t status_b, rtc_time_t *out);

/* Seconds since 1970-01-01 00:00:00 UTC for a valid calendar time. */
uint32_t rtc_to_unix(const rtc_time_t *t);

#ifdef __is_kernel
/* The current time as Unix seconds (UTC), or 0 if the clock can't be read. */
uint32_t rtc_now(void);
#endif

#endif /* _KERNEL_RTC_H */
