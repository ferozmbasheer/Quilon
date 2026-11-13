/*
 * Quilon OS -- CMOS real-time clock (see kernel/rtc.h)
 */
#include <kernel/rtc.h>

static int bcd(int v) { return (v >> 4) * 10 + (v & 0x0F); }

int rtc_decode(const uint8_t raw[RTC_NREGS], uint8_t status_b, rtc_time_t *out)
{
    int sec  = raw[RTC_R_SEC];
    int min  = raw[RTC_R_MIN];
    int hour = raw[RTC_R_HOUR];
    int day  = raw[RTC_R_DAY];
    int mon  = raw[RTC_R_MON];
    int year = raw[RTC_R_YEAR];

    /* The PM flag sits in the hour's top bit, outside the BCD digits. */
    int pm = !(status_b & RTC_B_24H) && (hour & RTC_HOUR_PM);
    hour &= ~RTC_HOUR_PM;

    if (!(status_b & RTC_B_BINARY)) {
        sec = bcd(sec); min = bcd(min); hour = bcd(hour);
        day = bcd(day); mon = bcd(mon); year = bcd(year);
    }
    if (!(status_b & RTC_B_24H)) {            /* 12 AM = 0h, 12 PM = 12h */
        if (hour < 1 || hour > 12) return -1;
        hour = hour % 12 + (pm ? 12 : 0);
    }

    if (sec > 59 || min > 59 || hour > 23 || day < 1 || day > 31 ||
        mon < 1 || mon > 12 || year > 99)
        return -1;

    out->year = 2000 + year;
    out->mon  = mon;
    out->day  = day;
    out->hour = hour;
    out->min  = min;
    out->sec  = sec;
    return 0;
}

uint32_t rtc_to_unix(const rtc_time_t *t)
{
    /* Days since the epoch: H. Hinnant's days_from_civil, years >= 1970. */
    int      y   = t->year - (t->mon <= 2);
    int      era = y / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (uint32_t)((153 * (t->mon + (t->mon > 2 ? -3 : 9)) + 2) / 5
                              + t->day - 1);
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    uint32_t days = (uint32_t)era * 146097u + doe - 719468u;
    return days * 86400u + (uint32_t)(t->hour * 3600 + t->min * 60 + t->sec);
}

#ifdef __is_kernel
#include <kernel/spinlock.h>

#define CMOS_INDEX   0x70
#define CMOS_DATA    0x71
#define CMOS_REG_A   0x0A
#define CMOS_REG_B   0x0B
#define CMOS_A_UIP   0x80    /* update in progress: registers are changing */

static const uint8_t rtc_reg[RTC_NREGS] = { 0x00, 0x02, 0x04, 0x07, 0x08, 0x09 };

/* The index/data port pair is not atomic: two CPUs (or a CPU and an IRQ)
 * interleaving would read the wrong register. */
static spinlock_t rtc_lock;

static uint8_t cmos_read(uint8_t reg)
{
    uint8_t v;
    asm volatile("outb %0, %1" :: "a"(reg), "Nd"((uint16_t)CMOS_INDEX));
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"((uint16_t)CMOS_DATA));
    return v;
}

static void read_regs(uint8_t raw[RTC_NREGS])
{
    /* Bounded: a missing RTC must not hang the caller. */
    for (int spin = 0; spin < 100000 && (cmos_read(CMOS_REG_A) & CMOS_A_UIP);
         spin++)
        ;
    for (int i = 0; i < RTC_NREGS; i++) raw[i] = cmos_read(rtc_reg[i]);
}

uint32_t rtc_now(void)
{
    uint8_t a[RTC_NREGS], b[RTC_NREGS], status_b;
    int same = 0;

    uint32_t fl = spinlock_acquire_irqsave(&rtc_lock);
    /* An update can still start between the UIP check and the reads:
     * read until two passes agree. */
    read_regs(a);
    for (int tries = 0; tries < 5 && !same; tries++) {
        read_regs(b);
        same = 1;
        for (int i = 0; i < RTC_NREGS; i++)
            if (a[i] != b[i]) { same = 0; a[i] = b[i]; }
    }
    status_b = cmos_read(CMOS_REG_B);
    spinlock_release_irqrestore(&rtc_lock, fl);

    rtc_time_t t;
    if (!same || rtc_decode(a, status_b, &t) != 0) return 0;
    return rtc_to_unix(&t);
}
#endif /* __is_kernel */
