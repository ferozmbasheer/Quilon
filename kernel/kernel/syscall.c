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
#endif

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
     *   Returns: 0 — there is only one "process" in a single-task kernel.
     * ──────────────────────────────────────────────────────────────────────── */
    case SYS_GETPID:
        ret = 0;
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
    case SYS_EXIT:
        printf("\r\n[kernel] process exited (code %d)\r\n", (int)regs->ebx);
#ifdef __is_kernel
        if (exec_return_active) {
            /* Shell launched this program — longjmp back to shell_cmd_exec. */
            exec_longjmp(&exec_return_buf, 1);
            __builtin_unreachable();
        }
        /* No return context (e.g. direct ring3/syscall shell command). */
        for (;;)
            asm volatile("hlt");
        __builtin_unreachable();
#else
        /* Host/test build: halt is skipped; write return value and return. */
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
     * Unknown syscall
     * ──────────────────────────────────────────────────────────────────────── */
    default:
        printf("[kernel] unknown syscall %d\r\n", (int)regs->eax);
        ret = (uint32_t)-1;
        break;
    }

    regs->eax = ret;
}
