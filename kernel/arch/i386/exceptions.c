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
     * Page Fault (vector 14) -- demand paging + SIGSEGV delivery (section 9.2).
     *
     * The CPU pushes a non-zero error code (err_code):
     *   bit 0 (P):  0 = not-present fault,  1 = protection fault.
     *   bit 1 (W):  0 = read fault,          1 = write fault.
     *   bit 2 (U):  0 = supervisor access,   1 = user access.
     *
     * CR2 always holds the faulting virtual address.
     *
     * Decision tree: see vma_fault_action() in vma.c.  In short, for a user
     * address inside a VMA, a not-present fault demand-pages a zeroed page and
     * a write to a present page in a writable VMA resolves copy-on-write --
     * whether the access came from ring 3 or from ring 0 inside a syscall.
     * Anything else is SIGSEGV from ring 3 and a panic from ring 0.
     */
    if (regs->int_no == 14 && current_process) {
        uint32_t fault_addr;
        asm volatile("mov %%cr2, %0" : "=r"(fault_addr));

        /* The address-space owner holds the authoritative VMA table.  For a
         * CLONE_VM thread that is the thread-group leader: the thread's own VMA
         * copy is a stale snapshot from clone time and will miss heap regions
         * the leader grew afterwards.  Page mapping still uses the faulting
         * process's cr3, which is shared within the group. */
        process_t *vma_owner = process_group_leader(current_process);
        int        user_mode = (regs->cs & 3) == 3;
        uint32_t  *proc_pd   = (uint32_t *)(uintptr_t)current_process->cr3;

        /* Ring-0 faults on user addresses are serviced too (REVIEW.md A/B):
         * a syscall writing into a not-yet-touched demand-paged buffer, or
         * (CR0.WP is set) into a CoW page shared after fork.  The syscall layer
         * validated the range with uap_ok first, so a VMA covers it.  The
         * policy itself is vma_fault_action(), unit-tested on the host. */
        pf_action_t act = vma_fault_action(vma_owner->vmas, PROC_VMA_MAX,
                                           fault_addr, regs->err_code,
                                           user_mode);

        if (act == PF_ACT_COW) {
            /* Write to a present page in a writable VMA: resolve CoW. */
            act = (paging_cow_handle(proc_pd, fault_addr) == 0)
                  ? PF_ACT_DEMAND /* handled */ : pf_unresolved(user_mode);
            if (act == PF_ACT_DEMAND) return;   /* CPU re-executes the insn */
        } else if (act == PF_ACT_DEMAND) {
            /* Demand-page: allocate a zeroed data page (above the identity map)
             * and map it.  pmm_alloc_data_page zeroes it via a transient kmap,
             * so we must NOT touch `phys` directly here -- it may be >4 MiB.
             * Do NOT printf on success: this handler runs with IF=0 (interrupt
             * gate), so spinning on tty_lock held elsewhere would deadlock. */
            vma_t   *vma        = vma_find(vma_owner->vmas, PROC_VMA_MAX,
                                           fault_addr);
            uint32_t page_flags = PAGE_PRESENT | PAGE_USER;
            if (vma->flags & VMA_W) page_flags |= PAGE_WRITABLE;
            uint32_t fault_page = fault_addr & ~(PAGE_SIZE - 1u);

            void *phys = pmm_alloc_data_page();
            if (phys && paging_map_page_alloc_into(proc_pd, fault_page,
                                                   (uint32_t)(uintptr_t)phys,
                                                   page_flags) == 0)
                return;                         /* CPU re-executes the insn */
            if (phys) pmm_free_page(phys);
            printf("\r\n[pf] pid %d: out of memory paging in 0x%x\r\n",
                   (int)current_process->pid, (unsigned)fault_addr);
            act = pf_unresolved(user_mode);
        }

        if (act == PF_ACT_SEGV) {
            printf("\r\n[pf] pid %d: %s at 0x%x (EIP=0x%x) -> SIGSEGV\r\n",
                   (int)current_process->pid,
                   (regs->err_code & PF_ERR_PRESENT) ? "protection fault"
                                                     : "invalid access",
                   (unsigned)fault_addr, (unsigned)regs->eip);
            signal_send(current_process, SIGSEGV);
            signal_dispatch();
            __builtin_unreachable();
        }
        /* PF_ACT_PANIC: an unresolvable ring-0 fault -- fall through. */
        printf("\r\n[pf] kernel fault at 0x%x err=0x%x\r\n",
               (unsigned)fault_addr, (unsigned)regs->err_code);
    }
#endif

    printf("\r\n--- KERNEL PANIC ---\r\n");
    if (regs->int_no < 32)
        printf("Exception: %s (vector %d)\r\n",
               exception_messages[regs->int_no], (int)regs->int_no);
    else
        printf("Exception: unknown (vector %d)\r\n", (int)regs->int_no);

    printf("err_code=0x%x\r\n", (int)regs->err_code);
#ifdef __is_kernel
    if (regs->int_no == 14) {
        uint32_t cr2; asm volatile("mov %%cr2, %0" : "=r"(cr2));
        printf("CR2=0x%x\r\n", (unsigned)cr2);
    }
#endif
    printf("EIP=0x%x  CS=0x%x  EFLAGS=0x%x\r\n",
           (int)regs->eip, (int)regs->cs, (int)regs->eflags);
    printf("EAX=0x%x  EBX=0x%x  ECX=0x%x  EDX=0x%x\r\n",
           (int)regs->eax, (int)regs->ebx, (int)regs->ecx, (int)regs->edx);

    for (;;)
        asm volatile("hlt");
}
