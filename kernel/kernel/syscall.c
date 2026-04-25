/*
 * Quilon OS — System Call Dispatcher
 *
 * System calls are the only safe channel through which user-mode (ring-3)
 * code can ask the kernel to perform privileged operations.
 *
 * Mechanism
 * ─────────
 * User code executes `int $0x80`.  The CPU looks up vector 0x80 in the IDT.
 * Because we set DPL=3 on that gate, ring-3 code is allowed to trigger it
 * (a DPL=0 gate would raise a GPF).  The CPU then:
 *   1. Switches to ring 0 using esp0/ss0 from the TSS.
 *   2. Pushes ss, useresp, eflags, cs, eip onto the kernel stack.
 *   3. Jumps to int80_stub (boot.S).
 *
 * int80_stub saves all registers, loads the kernel data segment, and calls
 * syscall_handler with a pointer to the register save area.  On return,
 * int80_stub restores registers and `iret`s back to ring 3.
 *
 * Return value convention
 * ───────────────────────
 * syscall_handler writes the return value into regs->eax.  int80_stub's
 * subsequent `popa` restores EAX from that slot, so the caller sees the
 * return value in EAX after `int $0x80`.
 *
 * Adding a new syscall
 * ────────────────────
 * 1. Add a #define SYS_xxx constant to syscall.h.
 * 2. Add a case in the switch below.
 * 3. Document the register arguments next to the case.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <kernel/syscall.h>
#include <kernel/tty.h>

#ifdef __is_kernel
#include <kernel/interrupts.h>
#include <kernel/serial.h>
#include <kernel/vfs.h>
#include <kernel/usermode.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/elf.h>
#include <kernel/paging.h>
#include <kernel/pmm.h>
#include <kernel/signal.h>
#include <string.h>
#endif

/* Virtual address where the user heap starts (just above typical ELF range). */
#define USER_HEAP_START  0x00800000u

/* ── Kernel-only initialisation ──────────────────────────────────────────────
 * int80_stub and IDT[] only exist in the kernel build (boot.S / interrupts.c).
 * Guard them so that syscall.c compiles cleanly on the host for unit tests.  */
#ifdef __is_kernel

/* Assembly entry point defined in arch/i386/boot.S.
 * Saves registers, sets up kernel segments, and calls syscall_handler. */
extern void int80_stub(void);

/*
 * syscall_initialize — register the int $0x80 gate in the IDT.
 *
 * IDT_TYPE_USER_GATE = 0xEE = P=1 | DPL=3 | type=0xE (32-bit interrupt gate).
 *
 * DPL=3 is the critical detail: without it, `int $0x80` from ring-3 code
 * raises a General Protection Fault (vector 13) instead of entering the
 * kernel.  Hardware interrupts always bypass the DPL check, so only
 * software-triggered interrupts care about this field.
 *
 * Must be called after idt_initialize() has loaded the IDTR, because
 * idt_set_gate() writes into the IDT array whose base address was just
 * committed to the CPU.  Adding entries after lidt() is safe — the CPU
 * reads the table on each interrupt, not only at startup.
 */
void syscall_initialize(void)
{
    idt_set_gate(0x80, (uint32_t)(uintptr_t)int80_stub,
                 IDT_SELECTOR_KERNEL_CODE, IDT_TYPE_USER_GATE);
}

#endif /* __is_kernel */

/*
 * syscall_handler — C-level dispatcher for int $0x80.
 *
 * Called from int80_stub with a pointer to the register save area built on
 * the kernel stack.  Dispatch is on regs->eax (the syscall number).
 * The return value must be written into regs->eax before returning;
 * int80_stub's popa delivers it to the caller's EAX register.
 */
void syscall_handler(syscall_regs_t *regs)
{
    uint32_t ret = (uint32_t)-1; /* default: -1 = ENOSYS (no such syscall) */

    switch (regs->eax) {

    /* ────────────────────────────────────────────────────────────────────────
     * SYS_WRITE (1)
     *   EBX = fd    — FD_STDOUT (1) or FD_STDERR (2) go to the VGA terminal.
     *                 FD_STDIN (0) or any other fd → returns 0.
     *   ECX = buf   — pointer to the bytes to write (user-space address).
     *   EDX = len   — number of bytes.
     *   Returns: bytes written on success, 0 on unsupported fd / NULL buf.
     *
     * Security note: in a production OS you must validate that [buf, buf+len)
     * is entirely within user-accessible memory before touching it.  Here
     * the first 4 MiB is identity-mapped and user-accessible, so any pointer
     * in that range is safe to dereference.
     * ──────────────────────────────────────────────────────────────────────── */
    case SYS_WRITE: {
        const char *buf = (const char *)(uintptr_t)regs->ecx;
        uint32_t    len = regs->edx;

        if ((regs->ebx == FD_STDOUT || regs->ebx == FD_STDERR) &&
            buf != NULL) {
            terminal_write(buf, (size_t)len);
#ifdef __is_kernel
            /* Mirror to serial so the output appears in QEMU -serial logs. */
            for (uint32_t i = 0; i < len; i++)
                serial_putchar(buf[i]);
#endif
            ret = len;
        } else {
            ret = 0;
        }
        break;
    }

    /* ────────────────────────────────────────────────────────────────────────
     * SYS_GETPID (2)
     *   No arguments.
     *   Returns: the current process's PID.
     * ──────────────────────────────────────────────────────────────────────── */
    case SYS_GETPID:
#ifdef __is_kernel
        ret = current_process ? current_process->pid : 0;
#else
        ret = 0;
#endif
        break;

    /* ────────────────────────────────────────────────────────────────────────
     * SYS_EXIT (3)
     *   EBX = exit code.
     *   Does not return.
     *
     * Without a scheduler, "exit" means halting the CPU.  A future
     * implementation would mark the task as ZOMBIE/DEAD and call schedule()
     * to switch to the next runnable task.
     * ──────────────────────────────────────────────────────────────────────── */
    /* ────────────────────────────────────────────────────────────────────────
     * SYS_EXIT (3)
     *   EBX = exit code.
     *   Does not return.
     *
     * Marks the process ZOMBIE, wakes the parent if it is blocked in
     * wait(), and calls scheduler_yield() to hand off the CPU.  The
     * shell's exec_return_active path is kept for the longjmp-based
     * shell-exec flow (when the shell drives exec directly).
     * ──────────────────────────────────────────────────────────────────────── */
    case SYS_EXIT:
        printf("\r\n[kernel] pid %d exited (code %d)\r\n",
#ifdef __is_kernel
               current_process ? (int)current_process->pid : -1,
#else
               -1,
#endif
               (int)regs->ebx);
#ifdef __is_kernel
        if (exec_return_active) {
            /* Shell launched this program via exec_setjmp — longjmp back. */
            exec_longjmp(&exec_return_buf, 1);
            __builtin_unreachable();
        }
        if (current_process) {
            current_process->exit_code = (int)regs->ebx;
            current_process->state     = PROC_ZOMBIE;
            /* Wake the parent if it is sleeping in SYS_WAIT. */
            process_t *_parent = process_find(current_process->parent_pid);
            if (_parent && _parent->state == PROC_BLOCKED)
                _parent->state = PROC_READY;
            scheduler_yield();   /* never returns — process is ZOMBIE */
            __builtin_unreachable();
        }
        for (;;) asm volatile("hlt");
        __builtin_unreachable();
#else
        regs->eax = 0;
        return;
#endif

    /* ────────────────────────────────────────────────────────────────────────
     * SYS_OPEN (4)
     *   EBX = pointer to null-terminated path string (user-space address).
     *   Returns: file descriptor (>= VFS_FD_BASE) on success, -1 on failure.
     *
     * Security note: the path pointer is trusted here (same caveat as
     * SYS_WRITE — pointer validation is left for a future memory-map check).
     * ──────────────────────────────────────────────────────────────────────── */
    case SYS_OPEN: {
#ifdef __is_kernel
        const char *path = (const char *)(uintptr_t)regs->ebx;
        ret = (path != (void *)0) ? (uint32_t)vfs_open(path) : (uint32_t)-1;
#else
        ret = (uint32_t)-1;   /* filesystem not available in host build */
#endif
        break;
    }

    /* ────────────────────────────────────────────────────────────────────────
     * SYS_READ (5)
     *   EBX = fd (must be >= VFS_FD_BASE, i.e. a filesystem fd)
     *   ECX = buf — pointer to the receive buffer (user-space address).
     *   EDX = len — maximum bytes to read.
     *   Returns: bytes read (0 = EOF), or -1 on error.
     * ──────────────────────────────────────────────────────────────────────── */
    case SYS_READ: {
#ifdef __is_kernel
        void *buf = (void *)(uintptr_t)regs->ecx;
        ret = (buf != (void *)0)
                ? (uint32_t)vfs_read((int)regs->ebx, buf, regs->edx)
                : (uint32_t)-1;
#else
        ret = (uint32_t)-1;
#endif
        break;
    }

    /* ────────────────────────────────────────────────────────────────────────
     * SYS_CLOSE (6)
     *   EBX = fd.
     *   Returns: 0 on success, -1 on error.
     * ──────────────────────────────────────────────────────────────────────── */
    case SYS_CLOSE: {
#ifdef __is_kernel
        ret = (uint32_t)vfs_close((int)regs->ebx);
#else
        ret = (uint32_t)-1;
#endif
        break;
    }

    /* ────────────────────────────────────────────────────────────────────────
     * SYS_WAIT (7)
     *   EBX = child PID to wait for.
     *   ECX = pointer to int where exit code is written (may be 0/NULL).
     *   Returns: 0 on success, -1 if the child PID is unknown.
     *
     * Blocks the caller (sets state PROC_BLOCKED) and yields the CPU.
     * The SYS_EXIT handler wakes us when the child becomes ZOMBIE.
     * We then read the exit code and reap the child (set it PROC_UNUSED).
     * ──────────────────────────────────────────────────────────────────────── */
    case SYS_WAIT: {
#ifdef __is_kernel
        uint32_t   child_pid    = regs->ebx;
        int       *exit_code_p  = (int *)(uintptr_t)regs->ecx;
        process_t *_child       = process_find(child_pid);
        if (!_child) { ret = (uint32_t)-1; break; }

        /* Block until the child transitions to PROC_ZOMBIE. */
        current_process->state = PROC_BLOCKED;
        while (_child->state != PROC_ZOMBIE)
            scheduler_yield();

        if (exit_code_p) *exit_code_p = _child->exit_code;
        _child->state = PROC_UNUSED;   /* reap: free the slot */
        ret = 0;
#else
        ret = (uint32_t)-1;
#endif
        break;
    }

    /* ────────────────────────────────────────────────────────────────────────
     * SYS_EXEC (8)
     *   EBX = pointer to null-terminated path string.
     *   Returns: child PID on success, -1 on failure.
     *
     * Creates a new address space, loads the ELF into it, and adds a new
     * PROC_READY entry to the process table.  The child runs when the
     * scheduler picks it.  The caller can use SYS_WAIT to synchronise.
     * ──────────────────────────────────────────────────────────────────────── */
    case SYS_EXEC: {
#ifdef __is_kernel
        const char *path = (const char *)(uintptr_t)regs->ebx;
        if (!path) { ret = (uint32_t)-1; break; }

        /* 1. Allocate a new page directory (kernel half shared). */
        uint32_t *child_pd = paging_create_address_space();
        if (!child_pd) { ret = (uint32_t)-1; break; }

        /* 2. Load the ELF into the child's address space. */
        uint32_t _entry = elf_load_into(path, child_pd);
        if (!_entry) {
            pmm_free_page(child_pd);
            ret = (uint32_t)-1;
            break;
        }

        /* 3. Create the PCB and mark it READY for the scheduler. */
        process_t *_child = process_create(path, _entry,
                                           (uint32_t)(uintptr_t)child_pd);
        if (!_child) {
            pmm_free_page(child_pd);
            ret = (uint32_t)-1;
            break;
        }

        ret = _child->pid;
#else
        ret = (uint32_t)-1;
#endif
        break;
    }

    /* ────────────────────────────────────────────────────────────────────────
     * SYS_FORK (9)
     *   No arguments.
     *   Returns: child PID in parent, 0 in child, -1 on failure.
     *
     * Steps (see ROADMAP2.md section 6.2):
     *   1. Allocate child page directory (kernel half shared).
     *   2. Copy all user pages: paging_fork_address_space().
     *   3. Allocate child PCB via process_create().
     *   4. Copy parent's syscall iret frame into child's kernel stack.
     *   5. Patch child EAX = 0 (fork returns 0 in child).
     *   6. Install 5-word context_switch frame pointing to fork_child_return.
     *   7. Return child PID to parent (regs->eax set at end of handler).
     *
     * The child is placed in PROC_READY and will be scheduled by IRQ0.
     * When first scheduled, context_switch rets to fork_child_return (boot.S),
     * which restores the full register set and irets to the user's return EIP
     * with EAX == 0.
     * ──────────────────────────────────────────────────────────────────────── */
    case SYS_FORK: {
#ifdef __is_kernel
        if (!current_process) { ret = (uint32_t)-1; break; }

        /* 1. Allocate child page directory. */
        uint32_t *fork_pd = paging_create_address_space();
        if (!fork_pd) { ret = (uint32_t)-1; break; }

        /* 2. Copy user pages from parent's address space into child's. */
        uint32_t *parent_pd = (uint32_t *)(uintptr_t)current_process->cr3;
        if (paging_fork_address_space(parent_pd, fork_pd) != 0) {
            pmm_free_page(fork_pd);
            ret = (uint32_t)-1;
            break;
        }

        /* 3. Allocate PCB.  entry=0 because kernel_esp is set manually. */
        process_t *fork_child = process_create(
            current_process->name, 0, (uint32_t)(uintptr_t)fork_pd);
        if (!fork_child) {
            pmm_free_page(fork_pd);
            ret = (uint32_t)-1;
            break;
        }
        fork_child->parent_pid = current_process->pid;
        fork_child->heap_end   = current_process->heap_end;

        /* 4. Copy the 64-byte int80_stub frame from parent's kernel stack.
         *
         * regs points to the ds slot at (kstack_top - 64).  The frame
         * extends 64 bytes upward to kstack_top (including useresp/ss beyond
         * the struct).
         *
         * We copy to the corresponding position in the child's kernel stack
         * so the iret frame is at the same offset from the top.
         */
        uint8_t *child_ktop =
            fork_child->kernel_stack + sizeof(fork_child->kernel_stack);

        memcpy(child_ktop - 64, (const uint8_t *)regs, 64);

        /* 5. Patch child EAX = 0.  EAX is at offset 32 from the ds slot
         *    (i.e. 8 uint32_ts from the struct start: ds,edi,esi,ebp,esp,ebx,
         *     edx,ecx = 8 × 4 = 32 bytes before eax).                      */
        *((uint32_t *)(child_ktop - 64 + 32)) = 0u;

        /* 6. Build the 5-word context_switch frame just below the iret frame.
         *
         * context_switch pops edi, esi, ebx, ebp, then rets.
         * We pre-push them (high→low): fork_child_return, ebp=0, ebx=0, esi=0, edi=0
         */
        extern void fork_child_return(void);
        uint32_t *fork_sp = (uint32_t *)(child_ktop - 64);
        *(--fork_sp) = (uint32_t)(uintptr_t)fork_child_return;
        *(--fork_sp) = 0u;   /* ebp */
        *(--fork_sp) = 0u;   /* ebx */
        *(--fork_sp) = 0u;   /* esi */
        *(--fork_sp) = 0u;   /* edi — kernel_esp points here */
        fork_child->kernel_esp = (uint32_t)(uintptr_t)fork_sp;

        ret = fork_child->pid;
#else
        ret = (uint32_t)-1;
#endif
        break;
    }

    /* ────────────────────────────────────────────────────────────────────────
     * SYS_SBRK (10)
     *   EBX = increment (bytes, signed).  Negative increments shrink the heap.
     *   Returns: old program break (void*) on success, -1 on OOM.
     *
     * sbrk(0) is a no-op that returns the current break (standard Unix idiom
     * for querying the break without changing it).
     *
     * The heap starts at USER_HEAP_START (0x800000) the first time sbrk is
     * called for a process whose heap_end == 0.
     *
     * If current_process is NULL (ring-3 code called from exec_setjmp path),
     * a static anonymous break is used so the shell's `sbrk` command works
     * without a scheduler-managed process context.
     * ──────────────────────────────────────────────────────────────────────── */
    case SYS_SBRK: {
#ifdef __is_kernel
        int32_t  sbrk_inc = (int32_t)regs->ebx;

        /* Resolve the current break pointer. */
        static uint32_t anon_break = 0;   /* fallback when no PCB */
        uint32_t *break_ptr =
            current_process ? &current_process->heap_end : &anon_break;

        if (*break_ptr == 0)
            *break_ptr = USER_HEAP_START;

        uint32_t old_brk = *break_ptr;
        if (sbrk_inc == 0) { ret = old_brk; break; }  /* query only */

        uint32_t new_brk = old_brk + (uint32_t)sbrk_inc;

        /* Map into whichever page directory is currently loaded in CR3.
         * In the exec_setjmp shell path, CR3 is already proc_pd (switched
         * before usermode_enter).  Using paging_map_page_alloc() would write
         * into the kernel's global page_directory[] instead — wrong PD.     */
        uint32_t active_cr3;
        asm volatile("mov %%cr3, %0" : "=r"(active_cr3));
        uint32_t *active_pd = (uint32_t *)(uintptr_t)active_cr3;

        /* Allocate and map physical pages for the newly requested range.
         * Page-align the start so we do not re-map already-covered pages.   */
        uint32_t page_addr = old_brk & ~(PAGE_SIZE - 1u);
        while (page_addr < new_brk) {
            void *phys = pmm_alloc_page();
            if (!phys) { ret = (uint32_t)-1; goto sbrk_done; }

            if (paging_map_page_alloc_into(active_pd, page_addr,
                                           (uint32_t)(uintptr_t)phys,
                                           PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER) != 0) {
                pmm_free_page(phys);
                ret = (uint32_t)-1;
                goto sbrk_done;
            }
            page_addr += PAGE_SIZE;
        }
        *break_ptr = new_brk;
        ret = old_brk;
        sbrk_done: break;
#else
        ret = (uint32_t)-1;
        break;
#endif
    }

    /* ────────────────────────────────────────────────────────────────────────
     * SYS_SIGRETURN (11)
     *   No arguments.
     *   Called by the signal handler trampoline stub after the user handler
     *   returns.  Restores the original register context that was saved before
     *   the signal was delivered.
     *
     *   TODO: actual signal stack frame restore.  For now this is a no-op
     *   stub that satisfies the syscall table without crashing.
     * ──────────────────────────────────────────────────────────────────────── */
    case SYS_SIGRETURN:
        ret = 0;
        break;

    /* ────────────────────────────────────────────────────────────────────────
     * Unknown syscall
     * ──────────────────────────────────────────────────────────────────────── */
    default:
        printf("[kernel] unknown syscall %d\r\n", (int)regs->eax);
        ret = (uint32_t)-1;
        break;
    }

    regs->eax = ret;

    /* Deliver any pending signals after the syscall return value is set.
     * SYS_EXIT and scheduler_yield() are noreturn, so this only fires for
     * syscalls that return normally.  signal_dispatch() is a no-op when
     * current_process is NULL or has no pending signals.                   */
#ifdef __is_kernel
    signal_dispatch();
#endif
}
