/* Quilon user libc -- signal() (ROADMAP3 17.0.1).  kill/setfg/intr are plain
 * syscall stubs in syscall.S. */
#include <signal.h>

int _sys_signal(int sig, int disposition);

sighandler_t signal(int sig, sighandler_t handler)
{
    if (handler != SIG_DFL && handler != SIG_IGN)
        return SIG_ERR;                  /* user handlers: not delivered yet */
    int old = _sys_signal(sig, handler == SIG_IGN ? 1 : 0);
    if (old < 0) return SIG_ERR;
    return old ? SIG_IGN : SIG_DFL;
}
