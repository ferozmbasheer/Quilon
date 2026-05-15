/*
 * Quilon user-space libc -- pthread.h
 *
 * Minimal POSIX thread API built on top of SYS_CLONE.
 *
 * Each thread is a kernel process_t that shares the creator's address space
 * (CLONE_VM | CLONE_FS | CLONE_FILES).  Thread IDs are kernel PIDs.
 * Joining uses SYS_WAIT; exiting uses SYS_EXIT.
 *
 * Limitations (hobby-OS subset):
 *   - No per-thread errno.
 *   - No cancellation.
 *   - No thread-local storage.
 *   - Return values are truncated to int (passed via exit code).
 */

#ifndef _PTHREAD_H
#define _PTHREAD_H

typedef int pthread_t;
typedef int pthread_attr_t;   /* unused; accepted for API compatibility */

/*
 * pthread_create -- start a new thread running start_routine(arg).
 *
 * Allocates a PTHREAD_STACK_SIZE stack with malloc, sets up a trampoline
 * on that stack, and calls clone().  The thread is immediately runnable.
 *
 * tid  -- receives the new thread's ID (kernel PID) on success.
 * attr -- ignored (pass NULL).
 *
 * Returns 0 on success, -1 on failure (OOM or process table full).
 */
int pthread_create(pthread_t *tid, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg);

/*
 * pthread_join -- wait for the thread tid to finish.
 *
 * Blocks the caller until the thread exits via pthread_exit or returns
 * from start_routine (which calls pthread_exit implicitly).
 *
 * retval -- if non-NULL, receives the void* passed to pthread_exit.
 *          (Implemented as the low-order 32 bits of the exit code.)
 *
 * Returns 0 on success, -1 on error.
 */
int pthread_join(pthread_t tid, void **retval);

/*
 * pthread_exit -- terminate the calling thread with a return value.
 *
 * Does not return.  Threads that return from start_routine call this
 * implicitly via the trampoline.
 */
void pthread_exit(void *retval) __attribute__((noreturn));

/*
 * pthread_self -- return the calling thread's ID (kernel PID).
 */
pthread_t pthread_self(void);

/* Default per-thread stack size. */
#define PTHREAD_STACK_SIZE (64 * 1024)

#endif /* _PTHREAD_H */
