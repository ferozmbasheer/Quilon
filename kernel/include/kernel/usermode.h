#ifndef _KERNEL_USERMODE_H
#define _KERNEL_USERMODE_H

#include <stdint.h>

/* ── GDT selectors for ring-3 segments ──────────────────────────────────────
 *
 * GDT layout (see arch/i386/gdt.c):
 *   [0] null      [1] kernel code  [2] kernel data  [3] kernel stack
 *   [4] user code [5] user data    [6] user stack    [7] TSS
 *
 * Selector encoding: (index * 8) | TI=0 | RPL
 *   Kernel segments have RPL=0, user segments have RPL=3.
 */
#define USER_CS   0x23u   /* GDT[4] | RPL=3 — user code segment  */
#define USER_DS   0x2Bu   /* GDT[5] | RPL=3 — user data segment  */
#define TSS_SEL   0x38u   /* GDT[7] | RPL=0 — Task State Segment */

/* ── User-mode stack layout ─────────────────────────────────────────────────
 *
 * A single 4-KiB page in BSS, 4-KiB aligned.
 * Because the kernel identity-maps the first 4 MiB (virt == phys),
 * the virtual address of this page is also its physical address.
 * usermode_initialize() marks it PAGE_USER | PAGE_WRITABLE.
 */
#define USER_STACK_SIZE  4096u   /* one page                    */

/* ── exec_setjmp / exec_longjmp ─────────────────────────────────────────────
 *
 * Minimal save/restore used by shell_cmd_exec so that the shell regains
 * control after a user program calls SYS_EXIT.
 *
 * Layout (must match the offsets in boot.S):
 *   [0]  ebx  [4]  esi  [8]  edi  [12] ebp  [16] esp  [20] eip
 */
typedef struct {
    uint32_t ebx, esi, edi, ebp, esp, eip;
} exec_jmp_buf_t;

/* Returns 0 the first time (direct call); returns val (≥1) when exec_longjmp
 * fires.  Saves EBX, ESI, EDI, EBP, ESP, and the return address.           */
int exec_setjmp(exec_jmp_buf_t *buf);

/* Restore the registers saved by exec_setjmp and jump back to the saved EIP,
 * making exec_setjmp appear to return val (minimum 1).                      */
void exec_longjmp(exec_jmp_buf_t *buf, int val) __attribute__((noreturn));

/* Global state shared between shell.c and syscall.c.
 * exec_return_active is set to 1 by shell_cmd_exec before entering ring 3,
 * and cleared to 0 again when control returns (via SYS_EXIT longjmp).      */
extern exec_jmp_buf_t exec_return_buf;
extern int            exec_return_active;

/* ── Public API ──────────────────────────────────────────────────────────────
 *
 * Call order (in kernel_main or a shell command):
 *   1. usermode_initialize()   — once, after paging_initialize()
 *   2. usermode_enter(fn)      — to jump to ring 3; never returns
 */

/* Prepare for ring-3 execution:
 *   • Load the TSS into the CPU task register (ltr) so that the CPU can
 *     find the kernel stack (esp0/ss0) when a ring-3 exception fires.
 *   • Mark the first 4 MiB of the page table USER-accessible so that
 *     ring-3 demo code (which lives in kernel text) can execute.
 *     NOTE: a production OS would never do this — user pages would be
 *     isolated in a separate address space.  Here it is a teaching aid.
 *   • Mark the per-task user stack page writable + user-accessible.
 *
 * Must be called after paging_initialize() and gdt_initialize().
 */
void usermode_initialize(void);

/* Jump to ring 3 and begin executing user_func.
 *
 * Builds an artificial iret frame on the kernel stack:
 *   SS    = USER_DS  (user data selector, RPL=3)
 *   ESP   = top of user stack page
 *   EFLAGS = 0x202  (IF=1, reserved bit 1 always set)
 *   CS    = USER_CS  (user code selector, RPL=3)
 *   EIP   = user_func
 *
 * When iret detects CS.RPL > CPL (ring 3 > ring 0) it performs a
 * full privilege-level change: pops ESP and SS from the frame, then
 * resumes at EIP with CPL=3.
 *
 * This function never returns to the caller.
 * usermode_initialize() must be called first.
 */
void usermode_enter(void (*user_func)(void));

/* ── Ring-3 demo tasks ───────────────────────────────────────────────────────
 *
 * These are normal C functions compiled into the kernel image, but they
 * are designed to be called at CPL=3.  They demonstrate:
 *   - Unprivileged memory writes to the VGA framebuffer.
 *   - How the CPU enforces ring boundaries (GPF on `hlt`).
 */

/* Writes a banner to the VGA framebuffer then executes `hlt`, which is a
 * privileged instruction.  Ring-3 code is not allowed to halt the CPU;
 * the CPU raises a General Protection Fault (vector 13).  The kernel's
 * exception handler catches it and prints a diagnostic.               */
void user_task_demo(void);

/* A benign ring-3 spin task.  Does nothing but `pause` in a loop.
 * Useful for verifying scheduler integration without triggering a fault. */
void user_task_spin(void);

/* Demonstrates system calls from ring-3 code.
 *   1. Calls SYS_WRITE (int $0x80, eax=1) to print a message via the kernel.
 *   2. Calls SYS_GETPID (int $0x80, eax=2) to retrieve the process ID.
 *   3. Calls SYS_EXIT  (int $0x80, eax=3) to terminate cleanly.
 * Never returns — SYS_EXIT halts the CPU. */
void user_task_syscall(void);

/* Demonstrates SYS_SBRK (section 6.3) from ring-3 code.
 *   1. Calls SYS_SBRK(4096) to extend the heap by one page.
 *   2. Writes a sentinel value to the newly allocated memory and reads it back.
 *   3. Reports success or failure via SYS_WRITE.
 *   4. Calls SYS_EXIT(0) so the shell's exec_setjmp path can reclaim control. */
void user_task_sbrk(void);

/* Demonstrates SYS_FORK (section 6.2) from ring-3 code.
 *   Calls SYS_FORK; parent prints the child PID, child prints "child running".
 *   Both call SYS_EXIT(0).
 *
 *   NOTE: fork() only works when called from a scheduler-managed process
 *   (current_process != NULL).  Called via the shell's exec_setjmp path
 *   it returns -1.  See shell command `fork` which sets up a proper context. */
void user_task_fork(void);

#endif /* _KERNEL_USERMODE_H */
