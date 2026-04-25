#ifndef _KERNEL_INTERRUPTS_H
#define _KERNEL_INTERRUPTS_H

#include <stddef.h>
#include <stdint.h>

struct IDT_entry{
	unsigned short int offset_lowerbits;
	unsigned short int selector;
	unsigned char zero;
	unsigned char type_attr;
	unsigned short int offset_higherbits;
};

/* ── IDT gate type_attr constants ───────────────────────────────────────── */
/* P=1, DPL=0, type=0xE → 32-bit interrupt gate, kernel-only              */
#define IDT_TYPE_INTERRUPT_GATE  0x8E
/* P=1, DPL=0, type=0xF → 32-bit trap gate (does not clear IF flag)       */
#define IDT_TYPE_TRAP_GATE       0x8F
/* P=1, DPL=3, type=0xE → 32-bit interrupt gate, callable from user mode  */
#define IDT_TYPE_USER_GATE       0xEE
/* P=1, DPL=3, type=0xF → 32-bit trap gate, callable from user mode,
 * does NOT clear IF — interrupts stay enabled during the syscall handler  */
#define IDT_TYPE_USER_TRAP_GATE  0xEF

/* Kernel code segment selector (GDT entry 1 × 8 bytes) */
#define IDT_SELECTOR_KERNEL_CODE 0x08

void idt_initialize(void);

/* Set a single IDT gate */
static inline void idt_set_gate(int vector, uint32_t handler,
                                uint16_t selector, uint8_t type_attr)
{
    extern struct IDT_entry IDT[];
    IDT[vector].offset_lowerbits  = handler & 0xFFFF;
    IDT[vector].selector          = selector;
    IDT[vector].zero              = 0;
    IDT[vector].type_attr         = type_attr;
    IDT[vector].offset_higherbits = (handler >> 16) & 0xFFFF;
}

#endif
