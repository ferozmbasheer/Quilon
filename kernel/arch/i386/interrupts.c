#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <kernel/interrupts.h>

extern void outb(unsigned short port, unsigned char data);
extern char inb(unsigned short port);

struct IDT_entry IDT[256];

void idt_initialize(void) {
  /* Register CPU exception handlers (vectors 0-31) */
  extern int isr0(),  isr1(),  isr2(),  isr3(),  isr4(),  isr5(),  isr6(),
             isr7(),  isr8(),  isr9(),  isr10(), isr11(), isr12(), isr13(),
             isr14(), isr15(), isr16(), isr17(), isr18(), isr19(), isr20(),
             isr21(), isr22(), isr23(), isr24(), isr25(), isr26(), isr27(),
             isr28(), isr29(), isr30(), isr31();

  static void (*isr_stubs[])(void) = {
      (void(*)(void))isr0,  (void(*)(void))isr1,  (void(*)(void))isr2,
      (void(*)(void))isr3,  (void(*)(void))isr4,  (void(*)(void))isr5,
      (void(*)(void))isr6,  (void(*)(void))isr7,  (void(*)(void))isr8,
      (void(*)(void))isr9,  (void(*)(void))isr10, (void(*)(void))isr11,
      (void(*)(void))isr12, (void(*)(void))isr13, (void(*)(void))isr14,
      (void(*)(void))isr15, (void(*)(void))isr16, (void(*)(void))isr17,
      (void(*)(void))isr18, (void(*)(void))isr19, (void(*)(void))isr20,
      (void(*)(void))isr21, (void(*)(void))isr22, (void(*)(void))isr23,
      (void(*)(void))isr24, (void(*)(void))isr25, (void(*)(void))isr26,
      (void(*)(void))isr27, (void(*)(void))isr28, (void(*)(void))isr29,
      (void(*)(void))isr30, (void(*)(void))isr31,
  };
  for (int i = 0; i < 32; i++)
      idt_set_gate(i, (uint32_t)isr_stubs[i],
                   IDT_SELECTOR_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE);

  extern int load_idt();

  /* IRQ assembly stubs defined in boot.S via the IRQ_STUB macro */
  extern void irq0(void),  irq1(void),  irq2(void),  irq3(void);
  extern void irq4(void),  irq5(void),  irq6(void),  irq7(void);
  extern void irq8(void),  irq9(void),  irq10(void), irq11(void);
  extern void irq12(void), irq13(void), irq14(void), irq15(void);

  static void (*irq_stubs[16])(void) = {
      irq0,  irq1,  irq2,  irq3,
      irq4,  irq5,  irq6,  irq7,
      irq8,  irq9,  irq10, irq11,
      irq12, irq13, irq14, irq15,
  };

	unsigned long idt_address;
	unsigned long idt_ptr[2];

  /* Remap the PIC: master IRQs 0-7 → vectors 32-39, slave IRQs 8-15 → 40-47 */
	outb(0x20, 0x11);  /* ICW1: start initialisation */
  outb(0xA0, 0x11);
  outb(0x21, 0x20);  /* ICW2: master base vector = 32 */
  outb(0xA1, 40);    /* ICW2: slave base vector = 40  */
  outb(0x21, 0x04);  /* ICW3: master has slave on IRQ2 */
  outb(0xA1, 0x02);  /* ICW3: slave cascade ID = 2    */
  outb(0x21, 0x01);  /* ICW4: 8086 mode               */
  outb(0xA1, 0x01);
  outb(0x21, 0x0);   /* OCW1: unmask all interrupts   */
  outb(0xA1, 0x0);

  /* Register IRQ stubs in IDT vectors 32-47 via a loop */
  for (int i = 0; i < 16; i++)
      idt_set_gate(32 + i, (uint32_t)irq_stubs[i],
                   IDT_SELECTOR_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE);

	/* fill the IDT descriptor */
	idt_address = (unsigned long)IDT;
	idt_ptr[0] = (sizeof(struct IDT_entry) * 256) + ((idt_address & 0xffff) << 16);
	idt_ptr[1] = idt_address >> 16;

	load_idt(idt_ptr);
}

void irq0_handler(void) {
    outb(0x20, 0x20); //EOI
}
 
 unsigned char keyboard_map[128] =
{
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8',	/* 9 */
  '9', '0', '-', '=', '\b',	/* Backspace */
  '\t',			/* Tab */
  'q', 'w', 'e', 'r',	/* 19 */
  't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',	/* Enter key */
    0,			/* 29   - Control */
  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',	/* 39 */
 '\'', '`',   0,		/* Left shift */
 '\\', 'z', 'x', 'c', 'v', 'b', 'n',			/* 49 */
  'm', ',', '.', '/',   0,				/* Right shift */
  '*',
    0,	/* Alt */
  ' ',	/* Space bar */
    0,	/* Caps lock */
    0,	/* 59 - F1 key ... > */
    0,   0,   0,   0,   0,   0,   0,   0,
    0,	/* < ... F10 */
    0,	/* 69 - Num lock*/
    0,	/* Scroll Lock */
    0,	/* Home key */
    0,	/* Up Arrow */
    0,	/* Page Up */
  '-',
    0,	/* Left Arrow */
    0,
    0,	/* Right Arrow */
  '+',
    0,	/* 79 - End key*/
    0,	/* Down Arrow */
    0,	/* Page Down */
    0,	/* Insert Key */
    0,	/* Delete Key */
    0,   0,   0,
    0,	/* F11 Key */
    0,	/* F12 Key */
    0,	/* All other keys are undefined */
};

void irq1_handler(void) {
    outb(0x20, 0x20); //EOI

    uint8_t scancode = inb(0x60);

    /* Bit 7 set means key-release event — ignore it */
    if (scancode & 0x80)
        return;

    if (scancode >= sizeof(keyboard_map))
        return;

    char c = keyboard_map[scancode];
    if (c != 0)
        printf("%c", c);
}
 
void irq2_handler(void) {
          outb(0x20, 0x20); //EOI
}
 
void irq3_handler(void) {
          outb(0x20, 0x20); //EOI
}
 
void irq4_handler(void) {
          outb(0x20, 0x20); //EOI
}
 
void irq5_handler(void) {
          outb(0x20, 0x20); //EOI
}
 
void irq6_handler(void) {
          outb(0x20, 0x20); //EOI
}
 
void irq7_handler(void) {
          outb(0x20, 0x20); //EOI
}
 
void irq8_handler(void) {
          outb(0xA0, 0x20);
          outb(0x20, 0x20); //EOI          
}
 
void irq9_handler(void) {
          outb(0xA0, 0x20);
          outb(0x20, 0x20); //EOI
}
 
void irq10_handler(void) {
          outb(0xA0, 0x20);
          outb(0x20, 0x20); //EOI
}
 
void irq11_handler(void) {
          outb(0xA0, 0x20);
          outb(0x20, 0x20); //EOI
}
 
void irq12_handler(void) {
          outb(0xA0, 0x20);
          outb(0x20, 0x20); //EOI
}
 
void irq13_handler(void) {
          outb(0xA0, 0x20);
          outb(0x20, 0x20); //EOI
}
 
void irq14_handler(void) {
          outb(0xA0, 0x20);
          outb(0x20, 0x20); //EOI
}
 
void irq15_handler(void) {
          outb(0xA0, 0x20);
          outb(0x20, 0x20); //EOI
}
