#include <stdint.h>
#include <stdio.h>

/* Must mirror exactly what is on the stack when exception_handler is called:
 *   [esp] pushed by us:   pointer to registers (the push %esp before call)
 *   then:  ds (push %eax after mov %ds,%ax)
 *   then:  edi,esi,ebp,esp,ebx,edx,ecx,eax  (pusha)
 *   then:  int_no, err_code  (pushed by stub)
 *   then:  eip,cs,eflags[,useresp,ss]  (pushed by CPU on exception)
 */
typedef struct {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
} registers_t;

static const char *exception_messages[] = {
    "Division By Zero",           /* 0  */
    "Debug",                      /* 1  */
    "Non Maskable Interrupt",     /* 2  */
    "Breakpoint",                 /* 3  */
    "Overflow",                   /* 4  */
    "BOUND Range Exceeded",       /* 5  */
    "Invalid Opcode",             /* 6  */
    "Device Not Available",       /* 7  */
    "Double Fault",               /* 8  */
    "Coprocessor Segment Overrun",/* 9  */
    "Invalid TSS",                /* 10 */
    "Segment Not Present",        /* 11 */
    "Stack Fault",                /* 12 */
    "General Protection Fault",   /* 13 */
    "Page Fault",                 /* 14 */
    "Reserved",                   /* 15 */
    "x87 Floating-Point",         /* 16 */
    "Alignment Check",            /* 17 */
    "Machine Check",              /* 18 */
    "SIMD Floating-Point",        /* 19 */
    "Virtualization",             /* 20 */
    "Reserved",                   /* 21 */
    "Reserved",                   /* 22 */
    "Reserved",                   /* 23 */
    "Reserved",                   /* 24 */
    "Reserved",                   /* 25 */
    "Reserved",                   /* 26 */
    "Reserved",                   /* 27 */
    "Reserved",                   /* 28 */
    "Reserved",                   /* 29 */
    "Security Exception",         /* 30 */
    "Reserved",                   /* 31 */
};

void exception_handler(registers_t *regs)
{
    printf("\r\n--- KERNEL PANIC ---\r\n");
    if (regs->int_no < 32)
        printf("Exception: %s (vector %d)\r\n",
               exception_messages[regs->int_no], (int)regs->int_no);
    else
        printf("Exception: unknown (vector %d)\r\n", (int)regs->int_no);

    printf("err_code=0x%x\r\n", (int)regs->err_code);
    printf("EIP=0x%x  CS=0x%x  EFLAGS=0x%x\r\n",
           (int)regs->eip, (int)regs->cs, (int)regs->eflags);
    printf("EAX=0x%x  EBX=0x%x  ECX=0x%x  EDX=0x%x\r\n",
           (int)regs->eax, (int)regs->ebx, (int)regs->ecx, (int)regs->edx);

    for (;;)
        asm volatile("hlt");
}
