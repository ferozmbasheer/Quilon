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

#endif /* _UNISTD_H */
