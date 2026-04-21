/*
 * Quilon OS — User Mode (Ring 3) support
 *
 * This file implements the jump from kernel mode (CPL=0, ring 0) to user
 * mode (CPL=3, ring 3) using the x86 iret technique.
 *
 * Background
 * ──────────
 * x86 has four privilege levels called "rings".  Ring 0 is the most
 * privileged (the kernel); ring 3 is the least privileged (user programs).
 * Hardware enforces this boundary:
 *   - Ring-3 code cannot execute privileged instructions (HLT, CLI, IN/OUT…).
 *   - Ring-3 code cannot access pages whose U/S flag (PAGE_USER) is clear.
 *   - Any violation raises a General Protection Fault (vector 13).
 *
 * The only way to enter ring 3 from ring 0 is the IRET instruction when
 * given a CS value whose RPL field equals 3.  IRET then pops EIP, CS,
 * EFLAGS, ESP, and SS from the stack and resumes execution at ring 3.
 *
 * Returning from ring 3 to ring 0 requires a system call (INT/SYSCALL)
 * or a CPU exception.  When the exception fires, the CPU reads esp0/ss0
 * from the TSS to find a valid kernel stack, then pushes the ring-3 state
 * and jumps to the exception handler.
 *
 * TSS requirement
 * ───────────────
 * The Task State Segment (TSS) must be loaded into the CPU's Task Register
 * (TR) via the LTR instruction before any ring switch happens.  Without it
 * the CPU has no way to find the kernel stack on a ring-3 exception and will
 * immediately triple-fault.  gdt_initialize() already sets esp0/ss0 in the
 * TSS; usermode_initialize() calls LTR to activate it.
 *
 * Demo vs production
 * ──────────────────
 * For this demo we mark the entire first 4 MiB PAGE_USER so that ring-3
 * code can call kernel functions (printf, terminal_write …).  A real OS
 * would give each process its own page directory with only its own pages
 * mapped USER-accessible, keeping the kernel invisible from ring 3.
 */

#include <stdint.h>

#include <kernel/usermode.h>
#include <kernel/paging.h>

/* ── User-mode stack ─────────────────────────────────────────────────────────
 * One 4-KiB page in .bss, 4-KiB aligned.
 * The kernel identity-maps the first 4 MiB (virt == phys), so this page's
 * virtual address IS its physical address.
 * usermode_initialize() adds PAGE_USER | PAGE_WRITABLE to its PTE.
 */
static uint8_t user_stack_page[USER_STACK_SIZE]
    __attribute__((aligned(4096)));

void usermode_initialize(void)
{
    /* ── Step 1: load the TSS into the task register ────────────────────
     * LTR marks the TSS descriptor in the GDT as "busy" and stores the
     * selector in TR.  The CPU reads esp0/ss0 from this TSS whenever it
     * needs to switch to ring 0 (e.g. on a ring-3 exception).
     * Without this step any ring-3 fault causes an immediate triple-fault.
     */
    asm volatile("ltr %0" :: "r"((uint16_t)TSS_SEL));

    /* ── Step 2: make first 4 MiB user-accessible ───────────────────────
     * Our ring-3 demo functions live in the kernel text segment, which
     * sits inside the first 4 MiB identity-mapped region.  For the CPU
     * to let ring-3 code execute those pages, their PTEs must have the
     * PAGE_USER bit set.
     *
     * Production note: a real OS maps user code into a separate virtual
     * address range that the kernel does NOT share, preventing user code
     * from reading or executing kernel memory.
     */
    paging_set_user_access(0x00000000u, 0x00400000u);

    /* ── Step 3: make the user stack writable + user-accessible ─────────
     * The stack page is already covered by the first-4-MiB mapping above,
     * but we set it explicitly to document intent and to ensure PAGE_WRITABLE
     * is present (ring-3 needs to be able to push/pop on its stack).
     */
    uint32_t stack_base = (uint32_t)(uintptr_t)user_stack_page;
    paging_set_user_access(stack_base, stack_base + USER_STACK_SIZE);
}

void usermode_enter(void (*user_func)(void))
{
    /* Top of the user stack (stack grows downward; ESP points here
     * before the first push inside user_func).                          */
    uint32_t user_esp =
        (uint32_t)(uintptr_t)user_stack_page + USER_STACK_SIZE;

    /* Load user-data selector into the segment registers that IRET does
     * not restore automatically (DS, ES, FS, GS).  Without this they
     * would still hold 0x10 (kernel data), and ring-3 code accessing data
     * through those segments would get a General Protection Fault.       */
    asm volatile(
        "movw %0, %%ax   \n\t"
        "movw %%ax, %%ds \n\t"
        "movw %%ax, %%es \n\t"
        "movw %%ax, %%fs \n\t"
        "movw %%ax, %%gs \n\t"
        :: "i"(USER_DS) : "eax"
    );

    /* Build the iret frame and execute iret.
     *
     * For a privilege-level change (CS.RPL > current CPL), IRET pops:
     *   EIP    ← pushed last  (top of frame)
     *   CS     ← user code selector (RPL=3 signals the privilege change)
     *   EFLAGS ← 0x202: IF=1 (enable hardware interrupts), bit 1 always 1
     *   ESP    ← user stack pointer
     *   SS     ← user stack/data selector (RPL=3)
     *
     * Pushing in reverse order because the stack grows downward:
     *   push SS, push ESP, push EFLAGS, push CS, push EIP
     * Then IRET pops them in the order listed above.                     */
    asm volatile(
        "push %0   \n\t"    /* SS    = USER_DS                  */
        "push %1   \n\t"    /* ESP   = top of user stack        */
        "push $0x202\n\t"   /* EFLAGS: IF=1, reserved bit 1     */
        "push %2   \n\t"    /* CS    = USER_CS                  */
        "push %3   \n\t"    /* EIP   = user_func                */
        "iret      \n\t"
        ::
          "r"((uint32_t)USER_DS),
          "r"(user_esp),
          "r"((uint32_t)USER_CS),
          "r"((uint32_t)(uintptr_t)user_func)
        : "memory"
    );

    /* iret transfers control to ring 3 — this line is never reached. */
    __builtin_unreachable();
}

/* ── Ring-3 demo tasks ───────────────────────────────────────────────────────
 *
 * These functions are compiled into the kernel image and are called at CPL=3
 * by usermode_enter().  They demonstrate two things:
 *   1. Ring-3 code CAN write to user-accessible memory (VGA framebuffer).
 *   2. Ring-3 code CANNOT execute privileged instructions (→ GPF).
 */

void user_task_demo(void)
{
    /* Write a message directly to the VGA text-mode framebuffer.
     * The VGA buffer is at physical 0xB8000, which is inside the
     * identity-mapped first 4 MiB — and we have set PAGE_USER on it.
     * Each cell is a 16-bit value: high byte = attribute, low byte = char.
     * Attribute 0x2F = white-on-green (stands out clearly as ring-3 output). */
    volatile uint16_t *vga = (volatile uint16_t *)0x000B8000u;
    const char msg[] = "[RING 3] user_task_demo: running at CPL=3   ";
    for (int i = 0; msg[i] != '\0'; i++)
        vga[80 + i] = (uint16_t)((0x2Fu << 8) | (uint8_t)msg[i]);

    /* Now deliberately execute HLT.
     *
     * HLT is a privileged instruction (only ring 0 may halt the CPU).
     * Executing it at CPL=3 causes the CPU to raise:
     *   General Protection Fault — vector 13, error code 0
     *
     * The kernel's exception handler (exceptions.c) will catch this,
     * print a diagnostic including the faulting EIP (pointing here),
     * and then halt.  This is the expected, correct behaviour.         */
    asm volatile("hlt");

    /* Unreachable — the GPF fires before we return. */
    while (1) asm volatile("pause");
}

void user_task_spin(void)
{
    /* A benign ring-3 task: spin forever without triggering a fault.
     * `pause` hints to the CPU that this is a spin-wait loop, reducing
     * power consumption and pipeline contention.
     * Useful for validating that the ring-3 entry itself works without
     * deliberately triggering a General Protection Fault.              */
    while (1) asm volatile("pause");
}
