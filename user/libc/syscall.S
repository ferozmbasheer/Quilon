/*
 * Quilon user-space libc — syscall.S
 *
 * Raw int $0x80 system call stubs for the Quilon kernel ABI:
 *
 *   EAX = syscall number
 *   EBX = argument 1
 *   ECX = argument 2
 *   EDX = argument 3
 *   int $0x80
 *   return value in EAX
 *
 * Stack layout after `pushl %ebx` (which all multi-arg syscalls do to
 * preserve the callee-saved register EBX):
 *
 *   [esp+ 0]  saved EBX
 *   [esp+ 4]  return address
 *   [esp+ 8]  argument 1  (first C argument)
 *   [esp+12]  argument 2
 *   [esp+16]  argument 3
 *
 * For syscalls that do not push EBX the offsets are 4 bytes lower.
 *
 * Build: assembled with i686-elf-gcc -m32 -ffreestanding -c
 */

.section .text

/* ── write(int fd, const void *buf, int len) → int ──────────────────── */
.global write
write:
    pushl %ebx
    movl  $1,       %eax    /* SYS_WRITE */
    movl  8(%esp),  %ebx    /* fd */
    movl  12(%esp), %ecx    /* buf */
    movl  16(%esp), %edx    /* len */
    int   $0x80
    popl  %ebx
    ret

/* ── read(int fd, void *buf, int len) → int ─────────────────────────── */
.global read
read:
    pushl %ebx
    movl  $5,       %eax    /* SYS_READ */
    movl  8(%esp),  %ebx    /* fd */
    movl  12(%esp), %ecx    /* buf */
    movl  16(%esp), %edx    /* len */
    int   $0x80
    popl  %ebx
    ret

/* ── open(const char *path) → int fd ────────────────────────────────── */
.global open
open:
    pushl %ebx
    movl  $4,      %eax     /* SYS_OPEN */
    movl  8(%esp), %ebx     /* path */
    int   $0x80
    popl  %ebx
    ret

/* ── close(int fd) → int ─────────────────────────────────────────────── */
.global close
close:
    pushl %ebx
    movl  $6,      %eax     /* SYS_CLOSE */
    movl  8(%esp), %ebx     /* fd */
    int   $0x80
    popl  %ebx
    ret

/* ── getpid(void) → int ──────────────────────────────────────────────── */
.global getpid
getpid:
    movl  $2, %eax          /* SYS_GETPID */
    int   $0x80
    ret

/* ── exit(int code) — does not return ───────────────────────────────── */
.global exit
exit:
    movl  $3,      %eax     /* SYS_EXIT */
    movl  4(%esp), %ebx     /* exit code */
    int   $0x80
    /* kernel marks process ZOMBIE and yields — should never reach here */
.Lexit_spin:
    pause
    jmp   .Lexit_spin

/* ── sbrk(int increment) → void* ────────────────────────────────────── */
.global sbrk
sbrk:
    pushl %ebx
    movl  $10,     %eax     /* SYS_SBRK */
    movl  8(%esp), %ebx     /* increment */
    int   $0x80
    popl  %ebx
    ret

/* ── exec(const char *path) → int child_pid ─────────────────────────── */
.global exec
exec:
    pushl %ebx
    movl  $8,      %eax     /* SYS_EXEC */
    movl  8(%esp), %ebx     /* path */
    int   $0x80
    popl  %ebx
    ret

/* ── fork(void) → int (child_pid in parent, 0 in child, -1 on error) ── */
.global fork
fork:
    movl  $9, %eax          /* SYS_FORK */
    int   $0x80
    ret

/* ── wait(int pid, int *exit_code) → int ────────────────────────────── */
.global wait
wait:
    pushl %ebx
    movl  $7,       %eax    /* SYS_WAIT */
    movl  8(%esp),  %ebx    /* pid */
    movl  12(%esp), %ecx    /* exit_code pointer */
    int   $0x80
    popl  %ebx
    ret

/* ── readdir(unsigned int index, dirent_t *out) → int ───────────────── */
.global readdir
readdir:
    pushl %ebx
    movl  $12,      %eax    /* SYS_READDIR */
    movl  8(%esp),  %ebx    /* index */
    movl  12(%esp), %ecx    /* dirent_t* out */
    int   $0x80
    popl  %ebx
    ret

/* ── create(const char *path) → int ─────────────────────────────────── */
.global create
create:
    pushl %ebx
    movl  $13,     %eax     /* SYS_CREATE */
    movl  8(%esp), %ebx     /* path */
    int   $0x80
    popl  %ebx
    ret

/* ── fremove(const char *path) → int ────────────────────────────────── */
.global fremove
fremove:
    pushl %ebx
    movl  $14,     %eax     /* SYS_REMOVE */
    movl  8(%esp), %ebx     /* path */
    int   $0x80
    popl  %ebx
    ret

/* ── getticks(void) → unsigned int ──────────────────────────────────── */
.global getticks
getticks:
    movl  $15, %eax         /* SYS_GETTICKS */
    int   $0x80
    ret

/* ── gethz(void) → unsigned int ─────────────────────────────────────── */
.global gethz
gethz:
    movl  $16, %eax         /* SYS_GETHZ */
    int   $0x80
    ret
