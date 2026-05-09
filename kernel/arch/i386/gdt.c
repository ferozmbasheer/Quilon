
#include <string.h>

#include <kernel/gdt.h>

extern uint32_t stack_top;

void init_gdt_desc(uint32_t base, uint32_t limit, uint8_t acces, uint8_t other, struct gdtdesc *desc)
{
    desc->lim0_15 = (limit & 0xffff);
    desc->base0_15 = (base & 0xffff);
    desc->base16_23 = (base & 0xff0000) >> 16;
    desc->acces = acces;
    desc->lim16_19 = (limit & 0xf0000) >> 16;
    desc->other = (other & 0xf);
    desc->base24_31 = (base & 0xff000000) >> 24;
    return;
}

struct gdtdesc 	kgdt[GDTSIZE];	
struct gdtr 		kgdtr;	
struct tss 		default_tss;

/*
 * gdt_set_kernel_stack — update the TSS kernel stack pointer.
 *
 * The TSS esp0 field tells the CPU where to load ESP when it transitions
 * from ring-3 to ring-0 (on any interrupt, exception, or syscall).  With
 * per-process kernel stacks, this must be updated on every context switch
 * so that ring-3 interrupts land on the correct process's kernel_stack[].
 *
 * Because the GDT descriptor for the TSS points directly at default_tss,
 * writing to default_tss.esp0 takes effect immediately — the CPU reads it
 * on the next ring-3 → ring-0 transition.
 */
void gdt_set_kernel_stack(uint32_t esp0)
{
    default_tss.esp0 = esp0;
}

void gdt_initialize_ap(void)
{
    /* The BSP already built the GDT and copied it to the physical address
     * stored in kgdtr.  APs just reload the same descriptor table and
     * refresh all segment registers to pick up the 32-bit flat selectors.*/
    asm volatile(
        "lgdtl   (kgdtr)        \n\t"
        "movw    $0x10, %%ax    \n\t"
        "movw    %%ax,  %%ds    \n\t"
        "movw    %%ax,  %%es    \n\t"
        "movw    %%ax,  %%fs    \n\t"
        "movw    %%ax,  %%gs    \n\t"
        "movw    %%ax,  %%ss    \n\t"
        "ljmp    $0x08, $1f     \n\t"
        "1:                     \n\t"
        ::: "eax"
    );
}

void gdt_initialize(void)
{
    default_tss.debug_flag = 0x00;
    default_tss.io_map = 0x00;
    default_tss.esp0 = (uint32_t)&stack_top;
    default_tss.ss0 = 0x10; /* kernel data segment selector */

    /* Init gdt segments */
    init_gdt_desc(0x0,                   0x0,     0x0,            0x0,                 &kgdt[0]); /* null   */
    init_gdt_desc(0x0,                   0xFFFFF, GDT_KERNEL_CODE, GDT_FLAGS_32BIT_PAGE, &kgdt[1]); /* kcode */
    init_gdt_desc(0x0,                   0xFFFFF, GDT_KERNEL_DATA, GDT_FLAGS_32BIT_PAGE, &kgdt[2]); /* kdata */
    init_gdt_desc(0x0,                   0x0,     GDT_KERNEL_STACK, GDT_FLAGS_32BIT_PAGE, &kgdt[3]); /* kstack */

    init_gdt_desc(0x0,                   0xFFFFF, GDT_USER_CODE,  GDT_FLAGS_32BIT_PAGE, &kgdt[4]); /* ucode */
    init_gdt_desc(0x0,                   0xFFFFF, GDT_USER_DATA,  GDT_FLAGS_32BIT_PAGE, &kgdt[5]); /* udata */
    init_gdt_desc(0x0,                   0x0,     GDT_USER_STACK, GDT_FLAGS_32BIT_PAGE, &kgdt[6]); /* ustack */

    init_gdt_desc((uint32_t)&default_tss, 0x67,   GDT_TSS,        0x00,                &kgdt[7]); /* tss */

    /* Init gdtr structure */
    kgdtr.limit = GDTSIZE * 8;
    kgdtr.base = GDTBASE;

    /* copy the gdtr to its memory area */
    memcpy((char *) kgdtr.base, (char *) kgdt, kgdtr.limit);

    /* load the gdtr registry */
    asm("lgdtl (kgdtr)");

    /* Init segments */
    asm("   movw $0x10, %ax    \n \
            movw %ax, %ds    \n \
            movw %ax, %es    \n \
            movw %ax, %fs    \n \
            movw %ax, %gs    \n \
            ljmp $0x08, $next    \n \
            next:        \n");
}
