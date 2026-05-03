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
#include <kernel/syscall.h>

/* ── exec_setjmp / exec_longjmp global state ─────────────────────────────────
 * shell_cmd_exec sets exec_return_active = 1 before entering ring 3 and
 * expects exec_longjmp to fire when the user program calls SYS_EXIT.
 * syscall.c checks exec_return_active in the SYS_EXIT handler.
 */
exec_jmp_buf_t exec_return_buf;
int            exec_return_active = 0;

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
    /* ── Step 1: load the TSS into the task register (once only) ────────
     * LTR marks the TSS descriptor in the GDT as "busy".  Issuing LTR a
     * second time on a busy TSS raises a General Protection Fault, so we
     * guard this step with a static flag.
     */
    static int tss_loaded = 0;
    if (!tss_loaded) {
        asm volatile("ltr %0" :: "r"((uint16_t)TSS_SEL));
        tss_loaded = 1;
    }

    /* ── Step 2: make first 4 MiB user-accessible in the active PD ──────
     * paging_set_user_access() reads CR3, so it operates on whatever page
     * directory is currently loaded — the calling process's private PD.
     * This must run for EVERY process launch (not just the first), because
     * each process has its own PD and needs PAGE_USER on PD[0] before
     * ring-3 code can access the user stack in the first 4 MiB.
     */
    paging_set_user_access(0x00000000u, 0x00400000u);

    /* After higher-half: mark the kernel-high region user-accessible so that
     * ring-3 demo tasks (compiled into kernel text at 0xC01xxxxx) can execute.
     * PD[0] and PD[KERNEL_PD_IDX] share the same page table, so the PTEs are
     * already marked USER by the call above — this only adds PAGE_USER to
     * the PD[KERNEL_PD_IDX] entry itself.                                   */
    paging_set_user_access(KERNEL_OFFSET, KERNEL_OFFSET + 0x00400000u);
}

void usermode_enter_esp(void (*user_func)(void), uint32_t user_esp_top)
{
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
     *   ESP    ← user stack pointer (user_esp_top)
     *   SS     ← user stack/data selector (RPL=3)
     *
     * Pushing in reverse order because the stack grows downward:
     *   push SS, push ESP, push EFLAGS, push CS, push EIP
     * Then IRET pops them in the order listed above.                     */
    asm volatile(
        "push %0   \n\t"    /* SS    = USER_DS                  */
        "push %1   \n\t"    /* ESP   = user_esp_top             */
        "push $0x202\n\t"   /* EFLAGS: IF=1, reserved bit 1     */
        "push %2   \n\t"    /* CS    = USER_CS                  */
        "push %3   \n\t"    /* EIP   = user_func                */
        "iret      \n\t"
        ::
          "r"((uint32_t)USER_DS),
          "r"(user_esp_top),
          "r"((uint32_t)USER_CS),
          "r"((uint32_t)(uintptr_t)user_func)
        : "memory"
    );

    /* iret transfers control to ring 3 — this line is never reached. */
    __builtin_unreachable();
}

void usermode_enter(void (*user_func)(void))
{
    /* Demo tasks (user_task_demo etc.) use the static BSS stack page.   */
    uint32_t user_esp =
        (uint32_t)(uintptr_t)user_stack_page + USER_STACK_SIZE;
    usermode_enter_esp(user_func, user_esp);
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

void user_task_syscall(void)
{
    /* ── SYS_WRITE — print a message through the kernel ─────────────────────
     *
     * This is the correct way for user-mode code to output text:
     * instead of writing directly to the VGA buffer (which would work in
     * our permissive demo setup), we ask the kernel to do it via a syscall.
     * In a real OS, user code can never touch kernel or device memory
     * directly — syscalls are the only bridge.                           */
    const char msg[] = "[ring3] SYS_WRITE via int $0x80 : syscall works!\r\n";
    uint32_t   len   = (uint32_t)(sizeof(msg) - 1);
    uint32_t   ret;

    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"((uint32_t)SYS_WRITE),
          "b"((uint32_t)FD_STDOUT),
          "c"((uint32_t)(uintptr_t)msg),
          "d"(len)
        : "memory"
    );

    /* ── SYS_GETPID — retrieve the process ID ────────────────────────────────
     *
     * Returns 0 in this single-task kernel.  Demonstrates a zero-argument
     * syscall and shows that EAX is correctly delivered back to user code.  */
    uint32_t pid;
    asm volatile(
        "int $0x80"
        : "=a"(pid)
        : "a"((uint32_t)SYS_GETPID)
        : "memory"
    );

    /* Report the PID we got back — write the message via SYS_WRITE again. */
    if (pid == 0) {
        const char pid_ok[] = "[ring3] SYS_GETPID returned 0 (expected)\r\n";
        uint32_t pid_len = (uint32_t)(sizeof(pid_ok) - 1);
        asm volatile(
            "int $0x80"
            :: "a"((uint32_t)SYS_WRITE),
               "b"((uint32_t)FD_STDOUT),
               "c"((uint32_t)(uintptr_t)pid_ok),
               "d"(pid_len)
            : "memory"
        );
    }

    /* ── SYS_EXIT — terminate cleanly ────────────────────────────────────────
     *
     * Asks the kernel to end this process with exit code 0.
     * The kernel prints a termination message and halts the CPU.
     * This instruction is never reached.                                  */
    asm volatile(
        "int $0x80"
        :: "a"((uint32_t)SYS_EXIT), "b"(0u)
        : "memory"
    );

    /* Unreachable — SYS_EXIT halts the CPU. */
    while (1) asm volatile("pause");
}

void user_task_sbrk(void)
{
    /* ── SYS_SBRK — extend the heap by one page ─────────────────────────────
     *
     * sbrk(increment) returns the old program break on success or -1 on OOM.
     * Here we request one 4-KiB page and verify we can write to the memory. */

    const int PAGE = 4096;
    uint32_t old_brk;

    asm volatile(
        "int $0x80"
        : "=a"(old_brk)
        : "a"(10u),        /* SYS_SBRK = 10 */
          "b"((uint32_t)PAGE)
        : "memory"
    );

    if (old_brk == (uint32_t)-1) {
        const char fail[] = "[ring3] sbrk(4096) failed: out of memory\r\n";
        uint32_t dummy;
        asm volatile(
            "int $0x80"
            : "=a"(dummy)
            : "a"(1u), "b"(1u),
              "c"((uint32_t)(uintptr_t)fail),
              "d"((uint32_t)(sizeof(fail) - 1))
            : "memory"
        );
    } else {
        /* Write a sentinel value to the first word of the new page and
         * read it back to confirm the mapping is writable.              */
        volatile uint32_t *heap = (volatile uint32_t *)(uintptr_t)old_brk;
        *heap = 0xDEADBEEFu;

        if (*heap == 0xDEADBEEFu) {
            const char ok[] =
                "[ring3] sbrk: page allocated and writable (sentinel OK)\r\n";
            uint32_t dummy;
            asm volatile(
                "int $0x80"
                : "=a"(dummy)
                : "a"(1u), "b"(1u),
                  "c"((uint32_t)(uintptr_t)ok),
                  "d"((uint32_t)(sizeof(ok) - 1))
                : "memory"
            );
        } else {
            const char bad[] = "[ring3] sbrk: sentinel mismatch!\r\n";
            uint32_t dummy;
            asm volatile(
                "int $0x80"
                : "=a"(dummy)
                : "a"(1u), "b"(1u),
                  "c"((uint32_t)(uintptr_t)bad),
                  "d"((uint32_t)(sizeof(bad) - 1))
                : "memory"
            );
        }
    }

    /* SYS_EXIT(0) — return control to the shell via exec_longjmp. */
    asm volatile(
        "int $0x80"
        :: "a"(3u), "b"(0u)
        : "memory"
    );
    while (1) asm volatile("pause");
}

void user_task_fork(void)
{
    /* ── SYS_FORK — create a child process ──────────────────────────────────
     *
     * fork() returns:
     *   > 0 in the parent (child PID)
     *   = 0 in the child
     *   < 0 (i.e. (uint32_t)-1) on failure
     *
     * If called without a scheduler-managed context (current_process == NULL)
     * the kernel returns -1; we print an informative message.               */

    uint32_t fork_ret;
    asm volatile(
        "int $0x80"
        : "=a"(fork_ret)
        : "a"(9u)   /* SYS_FORK = 9 */
        : "memory"
    );

    if (fork_ret == (uint32_t)-1) {
        const char msg[] =
            "[ring3] fork() returned -1 "
            "(needs scheduler context; run via exec, not direct ring3)\r\n";
        uint32_t dummy;
        asm volatile(
            "int $0x80"
            : "=a"(dummy)
            : "a"(1u), "b"(1u),
              "c"((uint32_t)(uintptr_t)msg),
              "d"((uint32_t)(sizeof(msg) - 1))
            : "memory"
        );
    } else if (fork_ret == 0) {
        /* Child: fork returned 0. */
        const char msg[] = "[ring3] fork: I am the CHILD (fork returned 0)\r\n";
        uint32_t dummy;
        asm volatile(
            "int $0x80"
            : "=a"(dummy)
            : "a"(1u), "b"(1u),
              "c"((uint32_t)(uintptr_t)msg),
              "d"((uint32_t)(sizeof(msg) - 1))
            : "memory"
        );
    } else {
        /* Parent: fork returned child PID. */
        const char msg[] = "[ring3] fork: I am the PARENT (got child PID)\r\n";
        uint32_t dummy;
        asm volatile(
            "int $0x80"
            : "=a"(dummy)
            : "a"(1u), "b"(1u),
              "c"((uint32_t)(uintptr_t)msg),
              "d"((uint32_t)(sizeof(msg) - 1))
            : "memory"
        );
    }

    /* SYS_EXIT(0) */
    asm volatile(
        "int $0x80"
        :: "a"(3u), "b"(0u)
        : "memory"
    );
    while (1) asm volatile("pause");
}
