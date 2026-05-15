/*
 * Quilon user-space libc -- pthread.c
 *
 * Implements the minimal POSIX thread API using SYS_CLONE.
 *
 * Thread creation protocol:
 *   1. pthread_create allocates a PTHREAD_STACK_SIZE block with malloc.
 *   2. It pushes arg and the real start_routine onto that stack so the
 *      trampoline can read them from known offsets.
 *   3. clone(_pthread_trampoline, sp, CLONE_VM|...) creates a kernel thread
 *      that starts at _pthread_trampoline with esp=sp.
 *   4. _pthread_trampoline calls start_routine(arg) then pthread_exit(NULL).
 *
 * Stack layout at the moment _pthread_trampoline begins executing (via iret):
 *
 *   esp+0  fake return address (NULL -- trampoline calls exit instead of ret)
 *   esp+4  start_routine function pointer
 *   esp+8  arg
 */

#include <pthread.h>
#include <unistd.h>    /* clone, exit, getpid, wait, SYS_CLONE, CLONE_* */
#include <stdlib.h>    /* malloc, free */

/* Defined in syscall.S -- a naked assembly stub that reads start_routine and
 * arg from the fixed stack offsets set up by pthread_create, calls
 * start_routine(arg), then calls exit(0).  A C function cannot be used here
 * because any compiler-generated prologue would shift esp before the offsets
 * are read. */
extern void _pthread_trampoline(void);

int pthread_create(pthread_t *tid, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg)
{
    (void)attr;

    char *stack = (char *)malloc(PTHREAD_STACK_SIZE);
    if (!stack)
        return -1;

    /* Set up the stack so _pthread_trampoline can read fn and arg.
     *
     * After all three pushes, esp points to the fake return address.
     * The trampoline reads:
     *   [esp+4] = start_routine
     *   [esp+8] = arg
     */
    void **sp = (void **)(stack + PTHREAD_STACK_SIZE);
    *(--sp) = arg;                              /* [esp+8] when trampoline runs */
    *(--sp) = (void *)(unsigned)start_routine;  /* [esp+4] when trampoline runs */
    *(--sp) = (void *)0;                        /* [esp+0] fake return address  */

    int new_tid = clone((void (*)(void))(unsigned)(void *)_pthread_trampoline,
                        (void *)sp,
                        CLONE_VM | CLONE_FS | CLONE_FILES);
    if (new_tid < 0) {
        free(stack);
        return -1;
    }

    if (tid)
        *tid = (pthread_t)new_tid;
    return 0;
}

int pthread_join(pthread_t tid, void **retval)
{
    int code = 0;
    if (wait((int)tid, &code) != 0)
        return -1;
    if (retval)
        *retval = (void *)(unsigned)code;
    return 0;
}

void __attribute__((noreturn)) pthread_exit(void *retval)
{
    exit((int)(unsigned)(retval));
    __builtin_unreachable();
}

pthread_t pthread_self(void)
{
    return (pthread_t)getpid();
}
