#ifndef _KERNEL_SYSCALL_H
#define _KERNEL_SYSCALL_H

#include <stdint.h>

/* ── System call numbers ─────────────────────────────────────────────────────
 *
 * Convention (mirrors the classic Linux i386 ABI):
 *   EAX = syscall number on entry; return value on exit
 *   EBX = argument 1
 *   ECX = argument 2
 *   EDX = argument 3
 *
 * User code invokes a system call with:
 *   asm volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), ...);
 */
#define SYS_WRITE   1   /* write(fd, buf, len) → bytes written              */
#define SYS_GETPID  2   /* getpid() → current process PID                   */
#define SYS_EXIT    3   /* exit(code) → does not return                      */
#define SYS_OPEN    4   /* open(path) → fd (>= 3) or -1                      */
#define SYS_READ    5   /* read(fd, buf, len) → bytes read, 0=EOF, -1=err    */
#define SYS_CLOSE   6   /* close(fd) → 0 or -1                               */
#define SYS_WAIT    7   /* wait(pid, &exit_code) → 0 on success, -1 on error */
#define SYS_EXEC    8   /* exec(path) → child PID on success, -1 on failure  */
#define SYS_FORK    9   /* fork() → child PID in parent, 0 in child, -1 err  */
#define SYS_SBRK    10  /* sbrk(increment) → old break (void*), or -1 on OOM */
#define SYS_SIGRETURN 11 /* sigreturn() — restore context after signal handler */
#define SYS_READDIR  12  /* readdir(index, dirent_buf) → 0 on success, -1 at end */
#define SYS_CREATE   13  /* create(path) → 0 on success, -1 on failure           */
#define SYS_REMOVE   14  /* remove(path) → 0 on success, -1 on failure           */
#define SYS_GETTICKS 15  /* getticks() → current PIT tick count (uint32_t)       */
#define SYS_GETHZ    16  /* gethz() → PIT frequency in Hz (uint32_t)             */
#define SYS_PIPE     17  /* pipe(int fds[2]) → 0 on success, -1 on failure       */
#define SYS_PCI_READ   18  /* pci_read(bus, (slot<<8)|func, offset) → 32-bit dword */
#define SYS_NET_SEND   19  /* net_send(buf, len) → 0 ok, -1 err                    */
#define SYS_NET_RECV   20  /* net_recv(buf, maxlen) → bytes copied, 0=none, -1=err */
#define SYS_NET_STATUS 21  /* net_status(mac6_buf) → 1=NIC ready, 0=not ready      */
#define SYS_NET_PING   22  /* net_ping(dst_ip) → 1=reply, 0=timeout, -1=err        */
#define SYS_NET_DHCP   23  /* net_dhcp() → 0=IP obtained, -1=timeout               */
#define SYS_NET_GETIP  24  /* net_getip() → host-order IPv4 address (0 if uncfg'd) */
#define SYS_VBE_INFO   25  /* vbe_info(uint32_t out[3]) → 1 if VBE active, 0 if not */
#define SYS_STAT    26  /* stat(path, vfs_stat_t*) → 0 or -1                      */
#define SYS_MKDIR   27  /* mkdir(path) → 0 or -1                                   */
#define SYS_CHDIR   28  /* chdir(path) → 0 or -1                                   */
#define SYS_GETCWD  29  /* getcwd(buf, len) → 0 or -1                              */
#define SYS_LSEEK   30  /* lseek(fd, offset, whence) → new position or -1         */
#define SYS_RENAME  31  /* rename(oldpath, newpath) → 0 or -1                      */

/* ── File descriptor numbers (used as EBX with SYS_WRITE) ───────────────────
 * FD_STDIN  → unsupported; SYS_WRITE returns 0.
 * FD_STDOUT / FD_STDERR → both map to the VGA terminal.
 */
#define FD_STDIN    0
#define FD_STDOUT   1
#define FD_STDERR   2

/* ── Register save-area ──────────────────────────────────────────────────────
 *
 * Mirrors the exact stack layout constructed by int80_stub in boot.S.
 * Fields are ordered from lowest to highest address (first field = [esp]).
 *
 * Full stack layout when syscall_handler is called (push %esp; call …):
 *
 *   [esp+ 0]  → (argument: pointer to this struct, added by `push %esp`)
 *   [esp+ 4]  ds           ← `push %eax` after `mov %ds, %ax`
 *   [esp+ 8]  edi          \
 *   [esp+12]  esi           |
 *   [esp+16]  ebp           |  `pusha` save area
 *   [esp+20]  (orig) esp    |    (pusha snapshots esp before the push)
 *   [esp+24]  ebx           |
 *   [esp+28]  edx           |
 *   [esp+32]  ecx           |
 *   [esp+36]  eax          /
 *   [esp+40]  int_no=0x80  \  pushed by int80_stub
 *   [esp+44]  err_code=0   /
 *   [esp+48]  eip          \
 *   [esp+52]  cs            |  pushed by CPU on int $0x80
 *   [esp+56]  eflags       /
 *   [esp+60]  useresp      \  only present on ring-3 → ring-0 transition
 *   [esp+64]  ss           /  handled transparently by `iret`
 *
 * The pointer arg is removed by `add $4, %esp` before popa, so `regs`
 * itself points to the `ds` field (first member of the struct).
 *
 * Returning a value: set regs->eax before syscall_handler returns.
 * int80_stub runs `popa` which restores all registers from the save area,
 * so the value in regs->eax becomes the caller's EAX.
 */
typedef struct {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; /* pusha save area */
    uint32_t int_no, err_code;                        /* pushed by stub  */
    uint32_t eip, cs, eflags;                         /* pushed by CPU   */
    /* useresp, ss follow on ring-3 entry — not in struct; iret uses them */
} syscall_regs_t;

/* ── Public API ──────────────────────────────────────────────────────────────
 *
 * Call order in kernel_main:
 *   idt_initialize();
 *   syscall_initialize();   ← must come after idt_initialize()
 *
 * syscall_initialize registers int $0x80 in the IDT with DPL=3 so that
 * ring-3 (user mode) code can invoke `int $0x80` without a GPF.
 */
void syscall_initialize(void);

/* Kernel-side dispatcher.  Called from int80_stub via `push %esp; call …`.
 * Dispatch is on regs->eax (syscall number); return value is stored in
 * regs->eax before returning so int80_stub's `popa` delivers it to the
 * caller's EAX.
 */
void syscall_handler(syscall_regs_t *regs);

#endif /* _KERNEL_SYSCALL_H */
