#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __is_kernel
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/scheduler.h>
#include <kernel/vma.h>
#include <kernel/paging.h>
#include <kernel/pmm.h>
#endif

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
#ifdef __is_kernel
    /*
     * Page Fault (vector 14) — demand paging + SIGSEGV delivery (section 9.2).
     *
     * The CPU pushes a non-zero error code (err_code):
     *   bit 0 (P):  0 = not-present fault,  1 = protection fault.
     *   bit 1 (W):  0 = read fault,          1 = write fault.
     *   bit 2 (U):  0 = supervisor access,   1 = user access.
     *
     * CR2 always holds the faulting virtual address.
     *
     * Decision tree:
     *
     *  [kernel-mode fault, bit 2 = 0]
     *    → kernel bug: fall through to panic below.
     *
     *  [user-mode fault, bit 0 = 1 (page present but protection violation)]
     *    → genuine fault (e.g. write to read-only page). → SIGSEGV.
     *    (Copy-on-write, section 9.3, will intercept this case first.)
     *
     *  [user-mode fault, bit 0 = 0 (page not present)]
     *    → demand paging candidate:
     *      - Look up fault_addr in the process's VMA list.
     *      - No VMA → out-of-bounds access → SIGSEGV.
     *      - VMA found → allocate a zero physical page, map it with the
     *        VMA's permission flags, and return.  The CPU re-executes the
     *        faulting instruction automatically.
     */
    if (regs->int_no == 14 && current_process) {
        uint32_t fault_addr;
        asm volatile("mov %%cr2, %0" : "=r"(fault_addr));

        /* Only handle user-mode faults here. */
        if ((regs->cs & 3) == 3) {

            /* Protection fault (page was present but access was denied). */
            if (regs->err_code & 1u) {
                /* Check for a copy-on-write write fault (section 9.3).
                 * Conditions: write fault (bit 1) + VMA says writable +
                 * PTE has PAGE_COW set.                                   */
                if (regs->err_code & 2u) {
                    vma_t *cow_vma = vma_find(current_process->vmas,
                                              PROC_VMA_MAX, fault_addr);
                    if (cow_vma && (cow_vma->flags & VMA_W)) {
                        uint32_t *proc_pd =
                            (uint32_t *)(uintptr_t)current_process->cr3;
                        if (paging_cow_handle(proc_pd, fault_addr) == 0) {
                            /* Handled — CPU re-executes the faulting insn. */
                            return;
                        }
                    }
                }
                printf("\r\n[pf] pid %d: protection fault at 0x%x "
                       "(EIP=0x%x) → SIGSEGV\r\n",
                       (int)current_process->pid,
                       (unsigned)fault_addr, (unsigned)regs->eip);
                signal_send(current_process, SIGSEGV);
                signal_dispatch();
                __builtin_unreachable();
            }

            /* Not-present fault — check VMAs. */
            vma_t *vma = vma_find(current_process->vmas, PROC_VMA_MAX, fault_addr);
            if (!vma) {
                printf("\r\n[pf] pid %d: no VMA at 0x%x "
                       "(EIP=0x%x) → SIGSEGV\r\n",
                       (int)current_process->pid,
                       (unsigned)fault_addr, (unsigned)regs->eip);
                signal_send(current_process, SIGSEGV);
                signal_dispatch();
                __builtin_unreachable();
            }

            /* Demand-page: allocate a zero physical page and map it. */
            void *phys = pmm_alloc_page();
            if (!phys) {
                printf("\r\n[pf] pid %d: OOM at 0x%x → SIGSEGV\r\n",
                       (int)current_process->pid, (unsigned)fault_addr);
                signal_send(current_process, SIGSEGV);
                signal_dispatch();
                __builtin_unreachable();
            }
            memset(phys, 0, PAGE_SIZE);

            uint32_t page_flags = PAGE_PRESENT | PAGE_USER;
            if (vma->flags & VMA_W) page_flags |= PAGE_WRITABLE;

            uint32_t fault_page = fault_addr & ~(PAGE_SIZE - 1u);
            uint32_t *proc_pd   = (uint32_t *)(uintptr_t)current_process->cr3;

            if (paging_map_page_alloc_into(proc_pd, fault_page,
                                           (uint32_t)(uintptr_t)phys,
                                           page_flags) != 0) {
                pmm_free_page(phys);
                printf("\r\n[pf] pid %d: mapping failed at 0x%x → SIGSEGV\r\n",
                       (int)current_process->pid, (unsigned)fault_addr);
                signal_send(current_process, SIGSEGV);
                signal_dispatch();
                __builtin_unreachable();
            }

            printf("[demand] pid %d: mapped page 0x%x\r\n",
                   (int)current_process->pid, (unsigned)fault_page);
            /* Return normally — CPU re-executes the faulting instruction. */
            return;
        }
        /* Kernel-mode page fault: fall through to panic. */
    }
#endif

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
