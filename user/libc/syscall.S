/*
 * Quilon user-space libc -- syscall.S
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

/* -- write(int fd, const void *buf, int len) -> int -------------------- */
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

/* -- read(int fd, void *buf, int len) -> int --------------------------- */
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

/* -- read_nonblock(int fd, void *buf, int len) -> int ---------------------- */
/* SYS_READ_NB (35): non-blocking; returns bytes read, 0 if none ready, -1 err */
.global read_nonblock
read_nonblock:
    pushl %ebx
    movl  $35,      %eax    /* SYS_READ_NB */
    movl  8(%esp),  %ebx    /* fd */
    movl  12(%esp), %ecx    /* buf */
    movl  16(%esp), %edx    /* len */
    int   $0x80
    popl  %ebx
    ret

/* -- open(const char *path) -> int fd ---------------------------------- */
.global open
open:
    pushl %ebx
    movl  $4,      %eax     /* SYS_OPEN */
    movl  8(%esp), %ebx     /* path */
    int   $0x80
    popl  %ebx
    ret

/* -- close(int fd) -> int ----------------------------------------------- */
.global close
close:
    pushl %ebx
    movl  $6,      %eax     /* SYS_CLOSE */
    movl  8(%esp), %ebx     /* fd */
    int   $0x80
    popl  %ebx
    ret

/* -- getpid(void) -> int ------------------------------------------------ */
.global getpid
getpid:
    movl  $2, %eax          /* SYS_GETPID */
    int   $0x80
    ret

/* -- exit(int code) -- does not return --------------------------------- */
.global exit
exit:
    movl  $3,      %eax     /* SYS_EXIT */
    movl  4(%esp), %ebx     /* exit code */
    int   $0x80
    /* kernel marks process ZOMBIE and yields -- should never reach here */
.Lexit_spin:
    pause
    jmp   .Lexit_spin

/* -- sbrk(int increment) -> void* -------------------------------------- */
.global sbrk
sbrk:
    pushl %ebx
    movl  $10,     %eax     /* SYS_SBRK */
    movl  8(%esp), %ebx     /* increment */
    int   $0x80
    popl  %ebx
    ret

/* -- exec(const char *path) -> int child_pid --------------------------- */
.global exec
exec:
    pushl %ebx
    movl  $8,      %eax     /* SYS_EXEC */
    movl  8(%esp), %ebx     /* path */
    int   $0x80
    popl  %ebx
    ret

/* -- exec_redir(const char *path, int in_fd, int out_fd) -> int ------------ */
/* SYS_EXEC_REDIR (36): exec with the child's stdin/stdout set to in_fd/out_fd, */
/* without touching the caller's own stdin/stdout. */
.global exec_redir
exec_redir:
    pushl %ebx
    movl  $36,      %eax    /* SYS_EXEC_REDIR */
    movl  8(%esp),  %ebx    /* path */
    movl  12(%esp), %ecx    /* in_fd */
    movl  16(%esp), %edx    /* out_fd */
    int   $0x80
    popl  %ebx
    ret

/* -- fork(void) -> int (child_pid in parent, 0 in child, -1 on error) -- */
.global fork
fork:
    movl  $9, %eax          /* SYS_FORK */
    int   $0x80
    ret

/* -- wait(int pid, int *exit_code) -> int ------------------------------ */
.global wait
wait:
    pushl %ebx
    movl  $7,       %eax    /* SYS_WAIT */
    movl  8(%esp),  %ebx    /* pid */
    movl  12(%esp), %ecx    /* exit_code pointer */
    int   $0x80
    popl  %ebx
    ret

/* -- readdir(unsigned int index, dirent_t *out) -> int ----------------- */
.global readdir
readdir:
    pushl %ebx
    movl  $12,      %eax    /* SYS_READDIR */
    movl  8(%esp),  %ebx    /* index */
    movl  12(%esp), %ecx    /* dirent_t* out */
    int   $0x80
    popl  %ebx
    ret

/* -- create(const char *path) -> int ----------------------------------- */
.global create
create:
    pushl %ebx
    movl  $13,     %eax     /* SYS_CREATE */
    movl  8(%esp), %ebx     /* path */
    int   $0x80
    popl  %ebx
    ret

/* -- fremove(const char *path) -> int ---------------------------------- */
.global fremove
fremove:
    pushl %ebx
    movl  $14,     %eax     /* SYS_REMOVE */
    movl  8(%esp), %ebx     /* path */
    int   $0x80
    popl  %ebx
    ret

/* -- getticks(void) -> unsigned int ------------------------------------ */
.global getticks
getticks:
    movl  $15, %eax         /* SYS_GETTICKS */
    int   $0x80
    ret

/* -- gethz(void) -> unsigned int --------------------------------------- */
.global gethz
gethz:
    movl  $16, %eax         /* SYS_GETHZ */
    int   $0x80
    ret

/* -- pipe(int fds[2]) -> int ------------------------------------------------ */
.global pipe
pipe:
    pushl %ebx
    movl  $17,      %eax    /* SYS_PIPE */
    movl  8(%esp),  %ebx    /* fds pointer */
    int   $0x80
    popl  %ebx
    ret

/* -- pci_read_u(bus, slot, func, off) -> unsigned int ---------------------- */
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

/* -- net_send(const void *buf, int len) -> int ----------------------------- */
.global net_send
net_send:
    pushl %ebx
    movl  $19,       %eax    /* SYS_NET_SEND */
    movl  8(%esp),   %ebx    /* buf */
    movl  12(%esp),  %ecx    /* len */
    int   $0x80
    popl  %ebx
    ret

/* -- net_recv(void *buf, int maxlen) -> int -------------------------------- */
.global net_recv
net_recv:
    pushl %ebx
    movl  $20,       %eax    /* SYS_NET_RECV */
    movl  8(%esp),   %ebx    /* buf */
    movl  12(%esp),  %ecx    /* maxlen */
    int   $0x80
    popl  %ebx
    ret

/* -- net_status(unsigned char mac[6]) -> int ------------------------------- */
.global net_status
net_status:
    pushl %ebx
    movl  $21,       %eax    /* SYS_NET_STATUS */
    movl  8(%esp),   %ebx    /* mac buffer (or NULL) */
    int   $0x80
    popl  %ebx
    ret

/* -- net_ping(unsigned int dst_ip) -> int ---------------------------------- */
.global net_ping
net_ping:
    pushl %ebx
    movl  $22,       %eax    /* SYS_NET_PING */
    movl  8(%esp),   %ebx    /* dst_ip (host byte order) */
    int   $0x80
    popl  %ebx
    ret

/* -- net_dhcp(void) -> int ------------------------------------------------- */
.global net_dhcp
net_dhcp:
    movl  $23, %eax          /* SYS_NET_DHCP */
    int   $0x80
    ret

/* -- net_getip(void) -> unsigned int --------------------------------------- */
.global net_getip
net_getip:
    movl  $24, %eax          /* SYS_NET_GETIP */
    int   $0x80
    ret

/* -- vbe_info_u(unsigned int out[3]) -> int -------------------------------- */
.global vbe_info_u
vbe_info_u:
    pushl %ebx
    movl  $25,      %eax     /* SYS_VBE_INFO */
    movl  8(%esp),  %ebx     /* out pointer */
    int   $0x80
    popl  %ebx
    ret

/* -- stat(const char *path, stat_t *out) -> int ---------------------------- */
.global stat
stat:
    pushl %ebx
    movl  $26,       %eax    /* SYS_STAT */
    movl  8(%esp),   %ebx    /* path */
    movl  12(%esp),  %ecx    /* stat_t* out */
    int   $0x80
    popl  %ebx
    ret

/* -- mkdir(const char *path) -> int --------------------------------------- */
.global mkdir
mkdir:
    pushl %ebx
    movl  $27,      %eax     /* SYS_MKDIR */
    movl  8(%esp),  %ebx     /* path */
    int   $0x80
    popl  %ebx
    ret

/* -- chdir(const char *path) -> int --------------------------------------- */
.global chdir
chdir:
    pushl %ebx
    movl  $28,      %eax     /* SYS_CHDIR */
    movl  8(%esp),  %ebx     /* path */
    int   $0x80
    popl  %ebx
    ret

/* -- getcwd(char *buf, unsigned int len) -> char* -------------------------- */
/* Returns buf on success (kernel writes path into buf and returns 0),       */
/* or NULL on failure.  Wrapper translates 0->buf, -1->NULL.                  */
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

/* -- lseek(int fd, int offset, int whence) -> int -------------------------- */
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

/* -- rename(const char *oldpath, const char *newpath) -> int --------------- */
.global rename
rename:
    pushl %ebx
    movl  $31,       %eax    /* SYS_RENAME */
    movl  8(%esp),   %ebx    /* oldpath */
    movl  12(%esp),  %ecx    /* newpath */
    int   $0x80
    popl  %ebx
    ret

/* -- clone(void (*fn)(void), void *stack, int flags) -> int ---------------- */
/* SYS_CLONE (32): create a kernel thread sharing the caller's address space. */
/* fn    = EBX: user-space entry point for the new thread.                    */
/* stack = ECX: prepared user-space stack pointer.                            */
/* flags = EDX: CLONE_VM | CLONE_FS | CLONE_FILES for a normal thread.       */
/* Returns new thread PID on success, -1 on failure.                          */
.global clone
clone:
    pushl %ebx
    movl  $32,       %eax    /* SYS_CLONE */
    movl  8(%esp),   %ebx    /* fn */
    movl  12(%esp),  %ecx    /* stack */
    movl  16(%esp),  %edx    /* flags */
    int   $0x80
    popl  %ebx
    ret

/* -- dup2(int oldfd, int newfd) -> int -------------------------------------- */
/* SYS_DUP2 (34): redirect fd 0, 1, or 2 to an existing pipe fd.             */
/* oldfd = EBX (existing pipe fd), newfd = ECX (0=stdin, 1/2=stdout/stderr). */
/* Returns 0 on success, -1 on error.                                         */
.global dup2
dup2:
    pushl %ebx
    movl  $34,       %eax    /* SYS_DUP2 */
    movl  8(%esp),   %ebx    /* oldfd */
    movl  12(%esp),  %ecx    /* newfd */
    int   $0x80
    popl  %ebx
    ret

/* -- mouse_read(mouse_event_t *out) -> int ---------------------------------- */
/* SYS_MOUSE_READ (33): fill *out with current mouse position and buttons.    */
/* Returns 1 on success, -1 if out is NULL.                                   */
.global mouse_read
mouse_read:
    pushl %ebx
    movl  $33,      %eax    /* SYS_MOUSE_READ */
    movl  8(%esp),  %ebx    /* mouse_event_t *out */
    int   $0x80
    popl  %ebx
    ret

/* -- proc_list(proc_info_t *buf, int max) -> int ---------------------------- */
/* SYS_PS (43): fill buf[0..max-1] with live-process info.                     */
/* Returns the number of entries written, or -1 on error.                     */
.global proc_list
proc_list:
    pushl %ebx
    movl  $43,      %eax    /* SYS_PS */
    movl  8(%esp),  %ebx    /* proc_info_t *buf */
    movl  12(%esp), %ecx    /* max */
    int   $0x80
    popl  %ebx
    ret

/* -- _pthread_trampoline -- ring-3 entry point for new pthreads ------------ */
/* Entered via iret from SYS_CLONE.  esp = clone_stack (no prologue).        */
/* pthread_create prepares the stack so:                                      */
/*   [esp+0]  NULL  fake return address                                       */
/*   [esp+4]  start_routine                                                   */
/*   [esp+8]  arg                                                             */
/* We read fn and arg from those fixed offsets, call fn(arg), then exit(0).  */
/* A C function cannot be used here because any compiler prologue would shift */
/* esp before we read the offsets, corrupting fn and arg.                     */
.global _pthread_trampoline
_pthread_trampoline:
    movl  4(%esp), %eax      /* eax = start_routine */
    movl  8(%esp), %ecx      /* ecx = arg           */
    pushl %ecx               /* push arg            */
    call  *%eax              /* call start_routine(arg) */
    addl  $4, %esp           /* pop arg             */
    pushl $0                 /* exit(0)             */
    call  exit
.Ltramp_spin:
    hlt
    jmp   .Ltramp_spin
