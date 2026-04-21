#include <stdint.h>
#include <kernel/pit.h>
#include <kernel/scheduler.h>

extern void outb(unsigned short port, unsigned char data);
extern char inb(unsigned short port);

/* PIT I/O ports */
#define PIT_DATA0   0x40   /* channel 0 data (read/write)          */
#define PIT_CMD     0x43   /* mode/command register (write-only)   */

/*
 * Command byte: channel 0 | lobyte/hibyte access | mode 3 (square wave) | binary
 * Bit layout: 7:6=00 (ch0), 5:4=11 (lo+hi), 3:1=011 (mode 3), 0=0 (binary)
 */
#define PIT_CMD_SQUAREWAVE  0x36

static volatile uint32_t pit_ticks = 0;
static          uint32_t pit_hz    = 0;

/*
 * pit_initialize — program PIT channel 0 to fire IRQ0 at `hz` per second.
 * Call once during kernel startup before enabling interrupts.
 */
void pit_initialize(uint32_t hz)
{
    pit_hz = hz;
    uint16_t divisor = pit_divisor(hz);
    outb(PIT_CMD,  PIT_CMD_SQUAREWAVE);
    outb(PIT_DATA0, (uint8_t)(divisor & 0xFF));          /* low byte  */
    outb(PIT_DATA0, (uint8_t)((divisor >> 8) & 0xFF));   /* high byte */
}

uint32_t pit_get_hz(void)
{
    return pit_hz;
}

/*
 * pit_tick — called from the IRQ0 handler on every timer interrupt.
 * Increments the tick counter and notifies the scheduler.
 */
void pit_tick(void)
{
    pit_ticks++;
    scheduler_tick();
}

/* Returns the number of timer ticks since pit_initialize(). */
uint32_t pit_get_ticks(void)
{
    return pit_ticks;
}

/*
 * pit_sleep_ticks — busy-wait for `ticks` timer interrupts.
 * Uses `hlt` to avoid spinning at full CPU speed; each IRQ wakes the CPU.
 */
void pit_sleep_ticks(uint32_t ticks)
{
    uint32_t target = pit_ticks + ticks;
    while (pit_ticks < target)
        asm volatile("hlt");
}
