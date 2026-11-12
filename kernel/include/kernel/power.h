/*
 * Quilon OS -- power off
 *
 * Real hardware powers off through ACPI: find the FADT, read PM1a_CNT_BLK,
 * look up the \_S5 sleep type in the DSDT's AML and write
 * SLP_TYPa | SLP_EN to PM1a_CNT.  Quilon doesn't parse ACPI tables yet, so
 * power_off() uses the fixed PM1a_CNT ports the common emulators expose
 * (their \_S5 sleep type is fixed too):
 *
 *   QEMU  (i440fx/PIIX4 PM, base 0x600)   outw(0x604,  0x2000)
 *   Bochs / QEMU < 2.0                    outw(0xB004, 0x2000)
 *   VirtualBox                            outw(0x4004, 0x3400)
 *
 * 0x2000 is SLP_EN (bit 13) with SLP_TYP = 0; VirtualBox's S5 type is 5
 * (5 << 10 = 0x1400).  If none of them takes, the machine is halted with
 * interrupts off and a message saying it is safe to switch off.
 */
#ifndef _KERNEL_POWER_H
#define _KERNEL_POWER_H

#include <stdint.h>

typedef struct {
    uint16_t    port;     /* PM1a control register     */
    uint16_t    value;    /* SLP_TYP << 10 | SLP_EN    */
    const char *name;
} power_method_t;

#define POWER_SLP_EN  0x2000u

static const power_method_t power_methods[] = {
    { 0x604,  0x2000, "QEMU"       },
    { 0xB004, 0x2000, "Bochs"      },
    { 0x4004, 0x3400, "VirtualBox" },
};
#define POWER_NMETHODS ((int)(sizeof power_methods / sizeof power_methods[0]))

/*
 * power_off -- switch the machine off.  Never returns: IRQs are masked, the
 * methods above are tried in turn, and if the machine is still running it
 * halts.  The caller should have said goodbye on the console already.
 */
void power_off(void) __attribute__((noreturn));

#endif
