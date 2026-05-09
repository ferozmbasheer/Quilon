/*
 * Quilon user-space libc — unistd.h
 *
 * Thin wrappers around Quilon's int $0x80 system call interface.
 * Each function loads the syscall number into EAX, arguments into
 * EBX/ECX/EDX, fires int $0x80, and returns EAX.
 *
 * Actual asm stubs live in syscall.S.  This header provides C
 * declarations so that user programs can call them normally.
 *
 * Syscall numbers match kernel/include/kernel/syscall.h exactly.
 */

#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>

/* ── Syscall numbers (mirror kernel/include/kernel/syscall.h) ─────────── */
#define SYS_WRITE     1
#define SYS_GETPID    2
#define SYS_EXIT      3
#define SYS_OPEN      4
#define SYS_READ      5
#define SYS_CLOSE     6
#define SYS_WAIT      7
#define SYS_EXEC      8
#define SYS_FORK      9
#define SYS_SBRK      10
#define SYS_SIGRETURN 11
#define SYS_READDIR   12
#define SYS_CREATE    13
#define SYS_REMOVE    14
#define SYS_GETTICKS  15
#define SYS_GETHZ     16
#define SYS_PIPE      17
#define SYS_PCI_READ  18  /* pci_read_u(bus,(slot<<8)|func,offset) → dword */
#define SYS_NET_SEND   19  /* net_send(buf, len) → 0 ok, -1 err            */
#define SYS_NET_RECV   20  /* net_recv(buf, maxlen) → bytes, 0=none, -1=err */
#define SYS_NET_STATUS 21  /* net_status(mac6_buf) → 1=ready, 0=not ready  */
#define SYS_NET_PING   22  /* net_ping(dst_ip) → 1=reply, 0=timeout, -1   */
#define SYS_NET_DHCP   23  /* net_dhcp() → 0=ok, -1=timeout               */
#define SYS_NET_GETIP  24  /* net_getip() → host-order IP (0 if not set)  */
#define SYS_VBE_INFO   25  /* vbe_info_u(uint32_t out[3]) → 1=active, 0=text */

/* ── Well-known file descriptors ──────────────────────────────────────── */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* ── File I/O ─────────────────────────────────────────────────────────── */
int  write(int fd, const void *buf, int len);
int  read(int fd, void *buf, int len);
int  open(const char *path);
int  close(int fd);

/* ── Process control ──────────────────────────────────────────────────── */
int  getpid(void);
void exit(int code) __attribute__((noreturn));
int  exec(const char *path);
int  fork(void);
int  wait(int pid, int *exit_code);

/* ── Heap control ─────────────────────────────────────────────────────── */
void *sbrk(int increment);

/* ── Directory enumeration ────────────────────────────────────────────── */
#include <dirent.h>
int readdir(unsigned int index, dirent_t *out);

/* ── Filesystem write operations ──────────────────────────────────────── */
int create(const char *path);   /* SYS_CREATE — create empty file, 0/-1 */
int fremove(const char *path);  /* SYS_REMOVE — delete file, 0/-1       */

/* ── Pipe (section 8.2) ──────────────────────────────────────────────────── */
int pipe(int fds[2]);           /* SYS_PIPE — create anonymous pipe, 0/-1 */

/* ── Timer ────────────────────────────────────────────────────────────────── */
unsigned int getticks(void);    /* SYS_GETTICKS — raw PIT tick counter   */
unsigned int gethz(void);       /* SYS_GETHZ    — PIT frequency in Hz    */

/* ── PCI configuration space (section 10.1) ──────────────────────────────── */
/* Read a 32-bit DWORD from PCI config space.
 * bus  : PCI bus number  (0–255)
 * slot : device slot     (0–31)
 * func : function number (0–7)
 * off  : byte offset (DWORD-aligned; low 2 bits ignored by hardware)
 * Returns: 32-bit config dword (vendor==0xFFFF → no device in slot)         */
unsigned int pci_read_u(unsigned int bus, unsigned int slot,
                        unsigned int func, unsigned int off);

/* ── Network (section 10.2) ───────────────────────────────────────────────── */
int net_send(const void *buf, int len);
int net_recv(void *buf, int maxlen);
int net_status(unsigned char mac[6]);   /* returns 1 if NIC ready, 0 if not */
int net_ping(unsigned int dst_ip);      /* ICMP echo; 1=reply 0=timeout -1=err */
int net_dhcp(void);                     /* DHCP discover→ack; 0=ok -1=timeout  */
unsigned int net_getip(void);           /* current IPv4 addr (0 if not set)    */

/* ── VBE framebuffer query (section 10.4) ─────────────────────────────────── */
/* Fills out[0]=width, out[1]=height, out[2]=bpp.
 * Returns 1 if VBE is active, 0 if in text mode.                             */
int vbe_info_u(unsigned int out[3]);

#endif /* _UNISTD_H */
