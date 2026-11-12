/*
 * Quilon OS -- power off (see kernel/power.h)
 */
#include <stdio.h>
#include <kernel/power.h>
#include <kernel/vbe.h>
#include <kernel/serial.h>

static inline void power_outw(uint16_t port, uint16_t val)
{
    asm volatile("outw %w0, %w1" :: "a"(val), "Nd"(port));
}

void power_off(void)
{
    /* Nothing may run after this point: no process, no IRQ handler.  The
     * filesystems write through (FAT16 writes each sector as it changes;
     * ext2 is read-only), so there is nothing to flush first. */
    asm volatile("cli" ::: "memory");
    static const char msg[] = "\r\n[kernel] powering off\r\n";
    serial_write(msg, sizeof msg - 1);

    for (int i = 0; i < POWER_NMETHODS; i++) {
        power_outw(power_methods[i].port, power_methods[i].value);
        /* An emulator acts on the write asynchronously; give it ~100 ms
         * (each port-0x80 write takes ~1 us) before trying the next one or
         * declaring failure.  IRQs are off, so the PIT can't time this. */
        for (int d = 0; d < 100000; d++)
            asm volatile("outb %%al, $0x80" :: "a"(0));
    }

    /* Still here: no emulator port answered (real hardware needs ACPI). */
    printf("\r\nIt is now safe to turn off your computer.\r\n");
    if (vbe_active()) vbe_flush();
    for (;;) asm volatile("cli; hlt");
}
