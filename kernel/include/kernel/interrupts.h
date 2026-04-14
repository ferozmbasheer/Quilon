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
