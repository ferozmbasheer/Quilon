/*
 * Quilon user libc -- <signal.h> (ROADMAP3 17.0.1)
 *
 * Quilon delivers signals but cannot yet run a user handler function: a
 * signal is either acted on with its default action (terminate) or ignored.
 * signal() therefore accepts only SIG_DFL and SIG_IGN and fails (SIG_ERR)
 * for a function pointer.
 */
#ifndef _USER_SIGNAL_H
#define _USER_SIGNAL_H

#define SIGINT    2     /* Ctrl-C */
#define SIGKILL   9     /* cannot be ignored */
#define SIGSEGV  11
#define SIGTERM  15     /* kill's default */
#define SIGCHLD  17

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

/* Set the disposition of sig; returns the previous one, or SIG_ERR. */
sighandler_t signal(int sig, sighandler_t handler);

/* Post sig to process pid.  0 or -1. */
int kill(int pid, int sig);

/* -- Quilon job control (no process groups / controlling tty yet) --------- */

/* Make child `pid` this process's foreground job, so Ctrl-C reaches it;
 * pid < 0 clears.  A shell calls setfg(child) before wait() and setfg(-1)
 * after.  0 or -1. */
int setfg(int pid);

/* SIGINT the foreground job of shell `shell_pid` (a child of the caller, or
 * the caller).  1 = sent, 0 = no job (pass the 0x03 on as input), -1 = error.
 * Used by the desktop terminal, which reads keys on the shell's behalf. */
int intr(int shell_pid);

#endif
