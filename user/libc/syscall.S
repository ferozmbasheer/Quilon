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

/* ── pipe(int fds[2]) → int ──────────────────────────────────────────────── */
.global pipe
pipe:
    pushl %ebx
    movl  $17,      %eax    /* SYS_PIPE */
    movl  8(%esp),  %ebx    /* fds pointer */
    int   $0x80
    popl  %ebx
    ret

/* ── pci_read_u(bus, slot, func, off) → unsigned int ────────────────────── */
/* SYS_PCI_READ: EBX=bus, ECX=(slot<<8)|func, EDX=offset                     */
.global pci_read_u
pci_read_u:
    pushl %ebx
    movl  $18,       %eax    /* SYS_PCI_READ */
    movl  8(%esp),   %ebx    /* arg1: bus */
    movl  12(%esp),  %ecx    /* arg2: slot */
    shll  $8,        %ecx    /* ecx = slot << 8 */
    orl   16(%esp),  %ecx    /* ecx |= func */
    movl  20(%esp),  %edx    /* arg4: offset */
    int   $0x80
    popl  %ebx
    ret

/* ── net_send(const void *buf, int len) → int ───────────────────────────── */
.global net_send
net_send:
    pushl %ebx
    movl  $19,       %eax    /* SYS_NET_SEND */
    movl  8(%esp),   %ebx    /* buf */
    movl  12(%esp),  %ecx    /* len */
    int   $0x80
    popl  %ebx
    ret

/* ── net_recv(void *buf, int maxlen) → int ──────────────────────────────── */
.global net_recv
net_recv:
    pushl %ebx
    movl  $20,       %eax    /* SYS_NET_RECV */
    movl  8(%esp),   %ebx    /* buf */
    movl  12(%esp),  %ecx    /* maxlen */
    int   $0x80
    popl  %ebx
    ret

/* ── net_status(unsigned char mac[6]) → int ─────────────────────────────── */
.global net_status
net_status:
    pushl %ebx
    movl  $21,       %eax    /* SYS_NET_STATUS */
    movl  8(%esp),   %ebx    /* mac buffer (or NULL) */
    int   $0x80
    popl  %ebx
    ret

/* ── net_ping(unsigned int dst_ip) → int ────────────────────────────────── */
.global net_ping
net_ping:
    pushl %ebx
    movl  $22,       %eax    /* SYS_NET_PING */
    movl  8(%esp),   %ebx    /* dst_ip (host byte order) */
    int   $0x80
    popl  %ebx
    ret

/* ── net_dhcp(void) → int ───────────────────────────────────────────────── */
.global net_dhcp
net_dhcp:
    movl  $23, %eax          /* SYS_NET_DHCP */
    int   $0x80
    ret

/* ── net_getip(void) → unsigned int ─────────────────────────────────────── */
.global net_getip
net_getip:
    movl  $24, %eax          /* SYS_NET_GETIP */
    int   $0x80
    ret

/* ── vbe_info_u(unsigned int out[3]) → int ──────────────────────────────── */
.global vbe_info_u
vbe_info_u:
    pushl %ebx
    movl  $25,      %eax     /* SYS_VBE_INFO */
    movl  8(%esp),  %ebx     /* out pointer */
    int   $0x80
    popl  %ebx
    ret

/* ── stat(const char *path, stat_t *out) → int ──────────────────────────── */
.global stat
stat:
    pushl %ebx
    movl  $26,       %eax    /* SYS_STAT */
    movl  8(%esp),   %ebx    /* path */
    movl  12(%esp),  %ecx    /* stat_t* out */
    int   $0x80
    popl  %ebx
    ret

/* ── mkdir(const char *path) → int ─────────────────────────────────────── */
.global mkdir
mkdir:
    pushl %ebx
    movl  $27,      %eax     /* SYS_MKDIR */
    movl  8(%esp),  %ebx     /* path */
    int   $0x80
    popl  %ebx
    ret

/* ── chdir(const char *path) → int ─────────────────────────────────────── */
.global chdir
chdir:
    pushl %ebx
    movl  $28,      %eax     /* SYS_CHDIR */
    movl  8(%esp),  %ebx     /* path */
    int   $0x80
    popl  %ebx
    ret

/* ── getcwd(char *buf, unsigned int len) → char* ────────────────────────── */
/* Returns buf on success (kernel writes path into buf and returns 0),       */
/* or NULL on failure.  Wrapper translates 0→buf, -1→NULL.                  */
.global getcwd
getcwd:
    pushl %ebx
    movl  $29,       %eax    /* SYS_GETCWD */
    movl  8(%esp),   %ebx    /* buf */
    movl  12(%esp),  %ecx    /* len */
    int   $0x80
    /* eax = 0 (ok) or -1 (err); convert to buf or NULL */
    testl %eax, %eax
    jl    .Lgetcwd_fail
    movl  8(%esp),   %eax    /* return buf */
    popl  %ebx
    ret
.Lgetcwd_fail:
    xorl  %eax, %eax         /* return NULL */
    popl  %ebx
    ret

/* ── lseek(int fd, int offset, int whence) → int ────────────────────────── */
.global lseek
lseek:
    pushl %ebx
    movl  $30,       %eax    /* SYS_LSEEK */
    movl  8(%esp),   %ebx    /* fd */
    movl  12(%esp),  %ecx    /* offset */
    movl  16(%esp),  %edx    /* whence */
    int   $0x80
    popl  %ebx
    ret

/* ── rename(const char *oldpath, const char *newpath) → int ─────────────── */
.global rename
rename:
    pushl %ebx
    movl  $31,       %eax    /* SYS_RENAME */
    movl  8(%esp),   %ebx    /* oldpath */
    movl  12(%esp),  %ecx    /* newpath */
    int   $0x80
    popl  %ebx
    ret
