#ifndef _KERNEL_PIT_H
#define _KERNEL_PIT_H

#include <stdint.h>

/* 8253/8254 PIT base oscillator frequency (Hz). */
#define PIT_BASE_HZ  1193180u

void     pit_initialize(uint32_t hz);
void     pit_tick(void);
uint32_t pit_get_ticks(void);
uint32_t pit_get_hz(void);
void     pit_sleep_ticks(uint32_t ticks);

/*
 * pit_divisor — compute the reload value for a given target frequency.
 *
 * The PIT channel 0 counter counts down from the divisor to 0 at
 * PIT_BASE_HZ ticks per second, then fires IRQ0.  Lower divisor →
 * higher IRQ rate.  Exposed as an inline so it can be unit-tested
 * without any hardware.
 */
static inline uint16_t pit_divisor(uint32_t hz)
{
    return (uint16_t)(PIT_BASE_HZ / hz);
}

#endif /* _KERNEL_PIT_H */
